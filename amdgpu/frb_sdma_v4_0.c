/*
 * sdma_v4_0.c (Linux 7.1.1, the kernel of the Vega 20 capture), for SDMA
 * 4.2.0 (Vega 20, the MI50): the two engines' gfx queues. Stage sdma runs
 * what amdgpu does for this block at load: early_init (num_instances, the
 * firmware headers: burst NOP), sw_init (the two rings, doorbells
 * sdma_engine[i] << 1), hw_init (golden registers, sdma_v4_0_start: unhalt,
 * context switch, per engine the gfx queue's resume, UTC L1, unhalt; the
 * ring tests), then the IB tests amdgpu runs from late_init
 * (amdgpu_ib_ring_tests). hw_fini halts both engines at unload. The
 * microcode is in the engines already: the PSP loaded SDMA0/SDMA1 in stage
 * smu, as on Linux (no sdma_v4_0_load_microcode).
 *
 * Also here: amdgpu_ttm's copy and fill for this engine (amdgpu_copy_buffer,
 * amdgpu_ttm_fill_mem on sdma_v4_0_emit_copy_buffer / _emit_fill_buffer:
 * SDMA 4's COPY_LINEAR and CONST_FILL take the count less one), polled.
 * frbctl `test sdma0|sdma1 ring|ib|copy|fill` runs the tests.
 *
 * NOT PORTED, on purpose:
 * - THE PAGE QUEUES. Vega 20's firmware (feature 42, version 145 >= 123)
 *   makes amdgpu set has_page_queue: each engine's second ring
 *   (SDMA0_PAGE_*, doorbell (sdma_engine[i] + 1) << 1) carries TTM's buffer
 *   moves on Linux. Firebrand's copies and fills go on the gfx queue (as on
 *   Polaris), so the page queues stay disabled as the reset left them
 *   (PAGE_RB_CNTL.RB_ENABLE 0) and the replay passes over page_resume.
 * - The RLC (compute) queues: Linux's sdma_v4_0_rlc_resume only does power
 *   gating, which Vega 20 does not have (no AMD_PG_SUPPORT_SDMA).
 * - Interrupts/IH (the trap, ECC, VM hole, doorbell and poll-timeout
 *   sources: SDMA0_CNTL.TRAP_ENABLE is never set, fences are polled), RAS,
 *   SR-IOV. (sdma_v4_0_setup_ulv and the clock gating of late_init are
 *   ported: the end of this file.)
 *
 * SOFT RESET (frb_sdma_v4_0_reset_funcs, for frb_reset.c): amdgpu's
 * sdma_v4_0_soft_reset is "todo" and returns 0; GC 9's GRBM_SOFT_RESET has
 * no SDMA bit (GC 10 got one) and Vega 20's SMU no SDMA reset message, so
 * Linux answers a hung SDMA 4.2 with a reset of the whole ASIC. Firebrand
 * resets the engine through SDMAn_VIRT_RESET_REQ.PF, the request the SR-IOV
 * host uses for the physical function. Measured on the MI50 (
 * 2026-11-15) on an engine whose UTCL1 write path was wedged: every
 * register of the block returns to its reset value (CLK_CTRL ff000100,
 * F32_CNTL HALT, the queue's registers 0), UTCL1_WR_STATUS and STATUS_REG
 * read idle, the microcode stays (UCODE_CHECKSUM unchanged, the F32 runs
 * when unhalted) and the other engine is not touched. So post_soft_reset
 * is hw_init again: the golden registers, sdma_v4_0_start, the late init.
 *
 * DEVIATIONS from amdgpu, each with its register-level consequence:
 * - PLACEMENT. Rings, writeback and IBs live in VRAM (frb_ring.c's objects
 *   region) until GTT exists; Linux keeps them in GTT. So GFX_RB_BASE(_HI),
 *   GFX_RB_RPTR_ADDR_HI/LO and GFX_RB_WPTR_POLL_ADDR_HI/LO carry other
 *   addresses. The WPTR writeback is followed by an HDP flush before the
 *   doorbell (it went in through BAR0).
 * - BYTE ORDER. Under __BIG_ENDIAN Linux would set RB_SWAP_ENABLE,
 *   RPTR_WRITEBACK_SWAP_ENABLE and IB_SWAP_ENABLE. They stay off: the ring,
 *   the IBs and the writeback are little-endian dwords written and read
 *   with explicit LE accessors, so every register value is the
 *   little-endian Linux capture's (docs/byte-order.md).
 * - The IB tests run at the end of the stage, not after every other block's
 *   hw_init as amdgpu_device_ip_late_init does; they touch no registers but
 *   the HDP flush/invalidate.
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
#include "frb_ring.h"
#include "frb_soc15.h"
#include "frb_ucode.h"
#include "include/asic_reg/sdma0/sdma0_4_2_offset.h"
#include "include/asic_reg/sdma0/sdma0_4_2_sh_mask.h"
#include "include/asic_reg/sdma1/sdma1_4_2_offset.h"
#include "include/asic_reg/sdma1/sdma1_4_2_sh_mask.h"
#include "include/asic_reg/nbio/nbio_7_4_offset.h"
#include "include/asic_reg/nbio/nbio_7_4_sh_mask.h"
#include "include/vega10_sdma_pkt_open.h"

#define SDMA_MAX_INSTANCE	2

/* amdgpu_drv.c module parameter default */
static const uint32_t amdgpu_sdma_phase_quantum = 32;

/* amdgpu_doorbell.h AMDGPU_VEGA20_DOORBELL_sDMA_ENGINE0/1 (vega20_doorbell_index_init) */
static const uint32_t vega20_sdma_engine_doorbell[SDMA_MAX_INSTANCE] = { 0x100, 0x10A };

/* the copy/fill tests: two buffers in the VRAM objects region */
#define FRB_SDMA_TEST_BYTES	(256u << 10)
#define FRB_SDMA_TEST_GUARD	256u	/* bytes left untouched at the end of the target */
#define FRB_SDMA_POISON		0xdeadd00du

/* amdgpu_sdma.h amdgpu_sdma_instance, the fields this port uses */
struct frb_sdma4_instance {
	struct frb_ring ring;
	uint32_t fw_version;
	uint32_t feature_version;
	int burst_nop;
};

/* amdgpu_sdma.h amdgpu_sdma */
struct frb_sdma4 {
	struct frb_sdma4_instance instance[SDMA_MAX_INSTANCE];
	int num_instances;
	int touched;			/* hw_init wrote the engines: hw_fini must halt them */
	uint32_t soft_reset;		/* check_soft_reset's verdict: bit i, engine i */
	uint32_t test_bo[2];		/* BAR0 offsets of the copy/fill buffers, 0 = none yet */
	uint32_t test_runs;
};

/* sdma_v4_0_get_reg_offset: instances 0 and 1 at their block's segment 0 */
static uint32_t sdma_v4_0_get_reg_offset(struct frb_device *adev, uint32_t instance, uint32_t offset)
{
	switch (instance) {
	case 0:
		return adev->reg_offset[SDMA0_HWIP][0][0] + offset;
	case 1:
		return adev->reg_offset[SDMA1_HWIP][0][0] + offset;
	default:
		return 0;
	}
}

#define RREG32_SDMA(instance, offset)	RREG32(sdma_v4_0_get_reg_offset(adev, (instance), (offset)))
#define WREG32_SDMA(instance, offset, value) \
	WREG32(sdma_v4_0_get_reg_offset(adev, (instance), (offset)), (value))

static const struct soc15_reg_golden golden_settings_sdma0_4_2_init[] = {
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC0_RB_WPTR_POLL_CNTL, 0xfffffff0, 0x00403000),
};

static const struct soc15_reg_golden golden_settings_sdma0_4_2[] =
{
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_CHICKEN_BITS, 0xfe931f07, 0x02831f07),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_CLK_CTRL, 0xffffffff, 0x3f000100),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_GB_ADDR_CONFIG, 0x0000773f, 0x00004002),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_GB_ADDR_CONFIG_READ, 0x0000773f, 0x00004002),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_GFX_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_GFX_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_PAGE_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_PAGE_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RD_BURST_CNTL, 0x0000000f, 0x00000003),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC0_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC0_RB_WPTR_POLL_CNTL, 0xfffffff0, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC1_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC1_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC2_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC2_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC3_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC3_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC4_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC4_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC5_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC5_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC6_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC6_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC7_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_RLC7_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_UTCL1_PAGE, 0x000003ff, 0x000003c0),
	SOC15_REG_GOLDEN_VALUE(SDMA0, 0, mmSDMA0_UTCL1_TIMEOUT, 0xffffffff, 0x00010001),
};

static const struct soc15_reg_golden golden_settings_sdma1_4_2[] = {
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_CHICKEN_BITS, 0xfe931f07, 0x02831f07),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_CLK_CTRL, 0xffffffff, 0x3f000100),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_GB_ADDR_CONFIG, 0x0000773f, 0x00004002),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_GB_ADDR_CONFIG_READ, 0x0000773f, 0x00004002),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_GFX_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_GFX_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_PAGE_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_PAGE_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RD_BURST_CNTL, 0x0000000f, 0x00000003),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC0_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC0_RB_WPTR_POLL_CNTL, 0xfffffff0, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC1_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC1_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC2_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC2_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC3_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC3_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC4_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC4_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC5_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC5_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC6_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC6_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC7_RB_RPTR_ADDR_LO, 0xfffffffd, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_RLC7_RB_WPTR_POLL_CNTL, 0xfffffff7, 0x00403000),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_UTCL1_PAGE, 0x000003ff, 0x000003c0),
	SOC15_REG_GOLDEN_VALUE(SDMA1, 0, mmSDMA1_UTCL1_TIMEOUT, 0xffffffff, 0x00010001),
};

/* include/linux/log2.h order_base_2: log2, rounded up */
static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

/* sdma_v4_0_init_golden_registers, IP_VERSION(4, 2, 0) */
static void sdma_v4_0_init_golden_registers(struct frb_device *adev)
{
	frb_soc15_program_register_sequence(adev, golden_settings_sdma0_4_2_init,
					    ARRAY_SIZE(golden_settings_sdma0_4_2_init));
	frb_soc15_program_register_sequence(adev, golden_settings_sdma0_4_2, ARRAY_SIZE(golden_settings_sdma0_4_2));
	frb_soc15_program_register_sequence(adev, golden_settings_sdma1_4_2, ARRAY_SIZE(golden_settings_sdma1_4_2));
}

/*
 * sdma_v4_0_init_microcode / amdgpu_sdma_init_microcode, the header half: the
 * PSP loaded the images (stage smu); the driver keeps the version and the
 * feature level, which decides burst NOPs (feature >= 20).
 */
static int sdma_v4_0_init_microcode(struct frb_device *adev, struct frb_sdma4 *sdma)
{
	const char *prefix = adev->row ? adev->row->fw_prefix : "vega20";
	char name[32];
	struct frb_fw fw;
	struct frb_ucode u;
	int err, i;

	for (i = 0; i < sdma->num_instances; i++) {
		frb_fw_name(name, sizeof(name), prefix, i == 0 ? "_sdma.bin" : "_sdma1.bin");
		fw.data = 0;
		fw.size = 0;
		err = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
		if (err) {
			frb_err(adev, "sdma_v4_0: Failed to load firmware \"%s\" (%d)", name, err);
			return err;
		}
		err = frb_ucode_parse(adev, name, &fw, &u);
		if (err)
			return err;
		if (u.header_size_bytes < FRB_FWH_SDMA_UCODE_FEATURE_VERSION + 4) {
			frb_err(adev, "%s: no sdma_firmware_header_v1_0", name);
			return -EINVAL;
		}
		sdma->instance[i].fw_version = u.ucode_version;
		sdma->instance[i].feature_version = frb_ucode_le32(&u, FRB_FWH_SDMA_UCODE_FEATURE_VERSION);
		if (sdma->instance[i].feature_version >= 20)
			sdma->instance[i].burst_nop = 1;
	}
	return 0;
}

static struct frb_sdma4_instance *amdgpu_sdma_get_instance_from_ring(struct frb_ring *ring)
{
	struct frb_sdma4 *sdma = ring->adev->sdma4;

	if (!sdma || ring->me < 0 || ring->me >= sdma->num_instances)
		return NULL;
	return &sdma->instance[ring->me];
}

/*
 * sdma_v4_0_ring_get_rptr: the engine writes its read pointer back as a
 * 64-bit byte offset (little-endian: Linux's "XXX check if swapping is
 * necessary on BE" is frb_wb_read64's LE decode)
 */
static uint64_t sdma_v4_0_ring_get_rptr(struct frb_ring *ring)
{
	return frb_wb_read64(ring->adev, ring->rptr_offs) >> 2;
}

/* sdma_v4_0_ring_get_wptr */
static uint64_t sdma_v4_0_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint64_t wptr;

	if (ring->use_doorbell) {
		wptr = frb_wb_read64(adev, ring->wptr_offs);
	} else {
		wptr = RREG32_SDMA(ring->me, mmSDMA0_GFX_RB_WPTR_HI);
		wptr = wptr << 32;
		wptr |= RREG32_SDMA(ring->me, mmSDMA0_GFX_RB_WPTR);
	}

	return wptr >> 2;
}

/*
 * sdma_v4_0_ring_set_wptr. The doorbell arm writes the WPTR writeback
 * (bytes, little-endian), flushes the HDP (VRAM through BAR0), then rings
 * the doorbell.
 */
static void sdma_v4_0_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	if (ring->use_doorbell) {
		frb_wb_write64(adev, ring->wptr_offs, ring->wptr << 2);
		frb_flush_hdp(adev);
		frb_doorbell_write64(adev, ring->doorbell_index, ring->wptr << 2);
	} else {
		WREG32_SDMA(ring->me, mmSDMA0_GFX_RB_WPTR, lower_32_bits(ring->wptr << 2));
		WREG32_SDMA(ring->me, mmSDMA0_GFX_RB_WPTR_HI, upper_32_bits(ring->wptr << 2));
	}
}

static void sdma_v4_0_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	struct frb_sdma4_instance *sdma = amdgpu_sdma_get_instance_from_ring(ring);
	uint32_t i;

	for (i = 0; i < count; i++)
		if (sdma && sdma->burst_nop && (i == 0))
			frb_ring_write(ring, ring->funcs->nop | SDMA_PKT_NOP_HEADER_COUNT(count - 1));
		else
			frb_ring_write(ring, ring->funcs->nop);
}

/* sdma_v4_0_ring_emit_ib - Schedule an IB on the DMA engine */
static void sdma_v4_0_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib, uint32_t vmid,
				   uint32_t flags)
{
	(void)flags;
	/* IB packet must end on a 8 DW boundary */
	sdma_v4_0_ring_insert_nop(ring, (2 - lower_32_bits(ring->wptr)) & 7);

	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT) |
		       SDMA_PKT_INDIRECT_HEADER_VMID(vmid & 0xf));
	/* base must be 32 byte aligned */
	frb_ring_write(ring, lower_32_bits(ib->gpu_addr) & 0xffffffe0);
	frb_ring_write(ring, upper_32_bits(ib->gpu_addr));
	frb_ring_write(ring, ib->length_dw);
	frb_ring_write(ring, 0);
	frb_ring_write(ring, 0);
}

static void sdma_v4_0_wait_reg_mem(struct frb_ring *ring, int mem_space, int hdp, uint32_t addr0,
				   uint32_t addr1, uint32_t ref, uint32_t mask, uint32_t inv)
{
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
		       SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(hdp) |
		       SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(mem_space) |
		       SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3)); /* == */
	if (mem_space) {
		/* memory */
		frb_ring_write(ring, addr0);
		frb_ring_write(ring, addr1);
	} else {
		/* registers */
		frb_ring_write(ring, addr0 << 2);
		frb_ring_write(ring, addr1 << 2);
	}
	frb_ring_write(ring, ref); /* reference */
	frb_ring_write(ring, mask); /* mask */
	frb_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
		       SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(inv)); /* retry count, poll interval */
}

/* sdma_v4_0_ring_emit_hdp_flush (nbio_v7_4's flush registers, SDMA0's bit << me) */
static void sdma_v4_0_ring_emit_hdp_flush(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t ref_and_mask = GPU_HDP_FLUSH_DONE__SDMA0_MASK << ring->me;

	sdma_v4_0_wait_reg_mem(ring, 0, 1,
			       SOC15_REG_OFFSET(NBIO, 0, mmGPU_HDP_FLUSH_DONE),
			       SOC15_REG_OFFSET(NBIO, 0, mmGPU_HDP_FLUSH_REQ),
			       ref_and_mask, ref_and_mask, 10);
}

/* sdma_v4_0_ring_emit_fence - the fence, then a TRAP (raises nothing: TRAP_ENABLE is off) */
static void sdma_v4_0_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq, unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;

	/* write the fence */
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE));
	/* zero in first two bits */
	frb_ring_write(ring, lower_32_bits(addr));
	frb_ring_write(ring, upper_32_bits(addr));
	frb_ring_write(ring, lower_32_bits(seq));

	/* optionally write high bits as well */
	if (write64bit) {
		addr += 4;
		frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE));
		frb_ring_write(ring, lower_32_bits(addr));
		frb_ring_write(ring, upper_32_bits(addr));
		frb_ring_write(ring, upper_32_bits(seq));
	}

	/* generate an interrupt */
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_TRAP));
	frb_ring_write(ring, SDMA_PKT_TRAP_INT_CONTEXT_INT_CONTEXT(0));
}

/* sdma_v4_0_gfx_enable - enable (or not) the gfx queues' RB and IB */
static void sdma_v4_0_gfx_enable(struct frb_device *adev, struct frb_sdma4 *sdma, int enable)
{
	uint32_t rb_cntl, ib_cntl;
	int i;

	for (i = 0; i < sdma->num_instances; i++) {
		rb_cntl = RREG32_SDMA(i, mmSDMA0_GFX_RB_CNTL);
		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, enable ? 1 : 0);
		WREG32_SDMA(i, mmSDMA0_GFX_RB_CNTL, rb_cntl);
		ib_cntl = RREG32_SDMA(i, mmSDMA0_GFX_IB_CNTL);
		ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, enable ? 1 : 0);
		WREG32_SDMA(i, mmSDMA0_GFX_IB_CNTL, ib_cntl);
	}
}

/*
 * sdma_v4_0_page_stop - the page queues off (has_page_queue on Vega 20; never
 * started here, so this only rewrites RB_ENABLE/IB_ENABLE 0, as Linux's
 * hw_fini does)
 */
static void sdma_v4_0_page_stop(struct frb_device *adev, struct frb_sdma4 *sdma)
{
	uint32_t rb_cntl, ib_cntl;
	int i;

	for (i = 0; i < sdma->num_instances; i++) {
		rb_cntl = RREG32_SDMA(i, mmSDMA0_PAGE_RB_CNTL);
		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_PAGE_RB_CNTL, RB_ENABLE, 0);
		WREG32_SDMA(i, mmSDMA0_PAGE_RB_CNTL, rb_cntl);
		ib_cntl = RREG32_SDMA(i, mmSDMA0_PAGE_IB_CNTL);
		ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_PAGE_IB_CNTL, IB_ENABLE, 0);
		WREG32_SDMA(i, mmSDMA0_PAGE_IB_CNTL, ib_cntl);
	}
}

/* sdma_v4_0_ctx_switch_enable - the engines' context switch (and phase quantum) */
static void sdma_v4_0_ctx_switch_enable(struct frb_device *adev, struct frb_sdma4 *sdma, int enable)
{
	uint32_t f32_cntl, phase_quantum = 0;
	int i;

	if (amdgpu_sdma_phase_quantum) {
		unsigned value = amdgpu_sdma_phase_quantum;
		unsigned unit = 0;

		while (value > (SDMA0_PHASE0_QUANTUM__VALUE_MASK >>
				SDMA0_PHASE0_QUANTUM__VALUE__SHIFT)) {
			value = (value + 1) >> 1;
			unit++;
		}
		if (unit > (SDMA0_PHASE0_QUANTUM__UNIT_MASK >>
			    SDMA0_PHASE0_QUANTUM__UNIT__SHIFT)) {
			value = (SDMA0_PHASE0_QUANTUM__VALUE_MASK >>
				 SDMA0_PHASE0_QUANTUM__VALUE__SHIFT);
			unit = (SDMA0_PHASE0_QUANTUM__UNIT_MASK >>
				SDMA0_PHASE0_QUANTUM__UNIT__SHIFT);
		}
		phase_quantum =
			value << SDMA0_PHASE0_QUANTUM__VALUE__SHIFT |
			unit  << SDMA0_PHASE0_QUANTUM__UNIT__SHIFT;
	}

	for (i = 0; i < sdma->num_instances; i++) {
		f32_cntl = RREG32_SDMA(i, mmSDMA0_CNTL);
		f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL, AUTO_CTXSW_ENABLE, enable ? 1 : 0);
		if (enable && amdgpu_sdma_phase_quantum) {
			WREG32_SDMA(i, mmSDMA0_PHASE0_QUANTUM, phase_quantum);
			WREG32_SDMA(i, mmSDMA0_PHASE1_QUANTUM, phase_quantum);
			WREG32_SDMA(i, mmSDMA0_PHASE2_QUANTUM, phase_quantum);
		}
		WREG32_SDMA(i, mmSDMA0_CNTL, f32_cntl);

		/* SDMA utilization (mmSDMA0_PUB_DUMMY_REG2): Arcturus only */
		/* Extend page fault timeout to avoid interrupt storm */
		WREG32_SDMA(i, mmSDMA0_UTCL1_TIMEOUT, 0x00800080);
	}
}

/* sdma_v4_0_enable - halt or unhalt the engines (the queues off first when halting) */
static void sdma_v4_0_enable(struct frb_device *adev, struct frb_sdma4 *sdma, int enable)
{
	uint32_t f32_cntl;
	int i;

	if (!enable) {
		sdma_v4_0_gfx_enable(adev, sdma, enable);
		/* sdma_v4_0_rlc_stop: empty */
		sdma_v4_0_page_stop(adev, sdma);	/* has_page_queue */
	}

	for (i = 0; i < sdma->num_instances; i++) {
		f32_cntl = RREG32_SDMA(i, mmSDMA0_F32_CNTL);
		f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, enable ? 0 : 1);
		WREG32_SDMA(i, mmSDMA0_F32_CNTL, f32_cntl);
	}
}

/* sdma_v4_0_rb_cntl - the ring size (no swap: little-endian rings, the file header) */
static uint32_t sdma_v4_0_rb_cntl(struct frb_ring *ring, uint32_t rb_cntl)
{
	/* Set ring buffer size in dwords */
	uint32_t rb_bufsz = order_base_2(ring->ring_size / 4);

	rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_SIZE, rb_bufsz);
	return rb_cntl;
}

/* sdma_v4_0_gfx_resume - set up and start one engine's gfx queue */
static void sdma_v4_0_gfx_resume(struct frb_device *adev, struct frb_sdma4 *sdma, unsigned int i)
{
	struct frb_ring *ring = &sdma->instance[i].ring;
	uint32_t rb_cntl, ib_cntl, wptr_poll_cntl;
	uint32_t doorbell;
	uint32_t doorbell_offset;
	uint64_t wptr_gpu_addr;

	frb_ring_clear(ring);

	rb_cntl = RREG32_SDMA(i, mmSDMA0_GFX_RB_CNTL);
	rb_cntl = sdma_v4_0_rb_cntl(ring, rb_cntl);
	WREG32_SDMA(i, mmSDMA0_GFX_RB_CNTL, rb_cntl);

	/* Initialize the ring buffer's read and write pointers */
	WREG32_SDMA(i, mmSDMA0_GFX_RB_RPTR, 0);
	WREG32_SDMA(i, mmSDMA0_GFX_RB_RPTR_HI, 0);
	WREG32_SDMA(i, mmSDMA0_GFX_RB_WPTR, 0);
	WREG32_SDMA(i, mmSDMA0_GFX_RB_WPTR_HI, 0);

	/* set the wb address whether it's enabled or not */
	WREG32_SDMA(i, mmSDMA0_GFX_RB_RPTR_ADDR_HI, upper_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFF);
	WREG32_SDMA(i, mmSDMA0_GFX_RB_RPTR_ADDR_LO, lower_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFC);

	rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);

	WREG32_SDMA(i, mmSDMA0_GFX_RB_BASE, (uint32_t)(ring->gpu_addr >> 8));
	WREG32_SDMA(i, mmSDMA0_GFX_RB_BASE_HI, (uint32_t)(ring->gpu_addr >> 40));

	ring->wptr = 0;

	/* before programing wptr to a less value, need set minor_ptr_update first */
	WREG32_SDMA(i, mmSDMA0_GFX_MINOR_PTR_UPDATE, 1);

	doorbell = RREG32_SDMA(i, mmSDMA0_GFX_DOORBELL);
	doorbell_offset = RREG32_SDMA(i, mmSDMA0_GFX_DOORBELL_OFFSET);

	doorbell = REG_SET_FIELD(doorbell, SDMA0_GFX_DOORBELL, ENABLE, ring->use_doorbell ? 1 : 0);
	doorbell_offset = REG_SET_FIELD(doorbell_offset, SDMA0_GFX_DOORBELL_OFFSET, OFFSET, ring->doorbell_index);
	WREG32_SDMA(i, mmSDMA0_GFX_DOORBELL, doorbell);
	WREG32_SDMA(i, mmSDMA0_GFX_DOORBELL_OFFSET, doorbell_offset);

	sdma_v4_0_ring_set_wptr(ring);

	/* set minor_ptr_update to 0 after wptr programed */
	WREG32_SDMA(i, mmSDMA0_GFX_MINOR_PTR_UPDATE, 0);

	/* setup the wptr shadow polling */
	wptr_gpu_addr = ring->wptr_gpu_addr;
	WREG32_SDMA(i, mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO, lower_32_bits(wptr_gpu_addr));
	WREG32_SDMA(i, mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI, upper_32_bits(wptr_gpu_addr));
	wptr_poll_cntl = RREG32_SDMA(i, mmSDMA0_GFX_RB_WPTR_POLL_CNTL);
	wptr_poll_cntl = REG_SET_FIELD(wptr_poll_cntl, SDMA0_GFX_RB_WPTR_POLL_CNTL, F32_POLL_ENABLE, 0);
	WREG32_SDMA(i, mmSDMA0_GFX_RB_WPTR_POLL_CNTL, wptr_poll_cntl);

	/* enable DMA RB */
	rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 1);
	WREG32_SDMA(i, mmSDMA0_GFX_RB_CNTL, rb_cntl);

	ib_cntl = RREG32_SDMA(i, mmSDMA0_GFX_IB_CNTL);
	ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 1);
	/* enable DMA IBs (no IB_SWAP_ENABLE: little-endian IBs) */
	WREG32_SDMA(i, mmSDMA0_GFX_IB_CNTL, ib_cntl);
}

/*
 * sdma_v4_0_ring_test_ring - the engine writes a value to a writeback dword
 * (VRAM here, read back after an HDP invalidate by frb_wb_read)
 */
static int sdma_v4_0_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	unsigned i;
	uint32_t index;
	int r;
	uint32_t tmp;
	uint64_t gpu_addr;

	r = frb_wb_get(adev, &index);
	if (r)
		return r;

	gpu_addr = frb_wb_gpu_addr(adev, index);
	tmp = 0xCAFEDEAD;
	frb_wb_write(adev, index, tmp);

	r = frb_ring_alloc(ring, 5);
	if (r)
		goto error_free_wb;

	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) |
		       SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR));
	frb_ring_write(ring, lower_32_bits(gpu_addr));
	frb_ring_write(ring, upper_32_bits(gpu_addr));
	frb_ring_write(ring, SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(0));
	frb_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = frb_wb_read(adev, index);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout) {
		frb_err(adev, "%s: ring test: the value is still 0x%08x after %u polls (rptr 0x%llx, wptr 0x%llx, "
			"SDMA%d_STATUS_REG %08x, F32_CNTL %08x, GFX_RB_RPTR %08x)",
			ring->name, tmp, i, (unsigned long long)ring->funcs->get_rptr(ring),
			(unsigned long long)ring->funcs->get_wptr(ring), ring->me,
			RREG32_SDMA(ring->me, mmSDMA0_STATUS_REG), RREG32_SDMA(ring->me, mmSDMA0_F32_CNTL),
			RREG32_SDMA(ring->me, mmSDMA0_GFX_RB_RPTR));
		r = -ETIMEDOUT;
	} else {
		frb_info(adev, "%s: ring test passed after %u polls (rptr 0x%llx, %s)", ring->name, i,
			 (unsigned long long)ring->funcs->get_rptr(ring),
			 ring->use_doorbell ? "doorbell" : "MMIO WPTR");
	}

error_free_wb:
	frb_wb_free(adev, index);
	return r;
}

/* sdma_v4_0_ring_test_ib - test an IB on the DMA engine (the fence polled) */
static int sdma_v4_0_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t index, seq = 0;
	uint32_t tmp = 0;
	uint64_t gpu_addr;
	int r;

	r = frb_wb_get(adev, &index);
	if (r)
		return r;

	gpu_addr = frb_wb_gpu_addr(adev, index);
	tmp = 0xCAFEDEAD;
	frb_wb_write(adev, index, tmp);
	memset(&ib, 0, sizeof(ib));
	r = frb_ib_get(adev, 256, &ib);
	if (r)
		goto err0;

	frb_ib_write(adev, &ib, SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) |
		     SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR));
	frb_ib_write(adev, &ib, lower_32_bits(gpu_addr));
	frb_ib_write(adev, &ib, upper_32_bits(gpu_addr));
	frb_ib_write(adev, &ib, SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(0));
	frb_ib_write(adev, &ib, 0xDEADBEEF);
	frb_ib_write(adev, &ib, SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP));
	frb_ib_write(adev, &ib, SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP));
	frb_ib_write(adev, &ib, SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP));

	r = frb_ib_schedule(ring, &ib, &seq);
	if (r)
		goto err0;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		/* the IB stays allocated: the engine may still fetch it */
		frb_err(adev, "%s: IB test: fence %u not signalled (fence 0x%08x, rptr 0x%llx)",
			ring->name, seq, frb_fence_read(ring),
			(unsigned long long)ring->funcs->get_rptr(ring));
		goto err0;
	}
	tmp = frb_wb_read(adev, index);
	if (tmp == 0xDEADBEEF) {
		r = 0;
		frb_info(adev, "%s: IB test passed (fence %u)", ring->name, seq);
	} else {
		frb_err(adev, "%s: IB test: fence %u signalled but the value is 0x%08x", ring->name,
			seq, tmp);
		r = -EINVAL;
	}
	frb_ib_free(adev, &ib);
err0:
	frb_wb_free(adev, index);
	return r;
}

/* amdgpu_ring_test_helper */
static int amdgpu_ring_test_helper(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	int r;

	r = sdma_v4_0_ring_test_ring(ring);
	if (r)
		frb_err(adev, "ring %s test failed (%d)", ring->name, r);
	else
		frb_dbg(adev, "ring test on %s succeeded", ring->name);

	ring->ready = !r;

	return r;
}

/*
 * sdma_v4_0_start, bare metal, PSP-loaded: unhalt, context switch, then per
 * engine the gfx queue (the page queue is not ported: the file header), UTC
 * L1 and the unhalt; sdma_v4_0_rlc_resume (power gating: none on Vega 20);
 * the ring tests.
 */
static int sdma_v4_0_start(struct frb_device *adev, struct frb_sdma4 *sdma)
{
	struct frb_ring *ring;
	int i, r = 0;

	/* unhalt the MEs */
	sdma_v4_0_enable(adev, sdma, 1);
	/* enable sdma ring preemption */
	sdma_v4_0_ctx_switch_enable(adev, sdma, 1);

	/* start the gfx rings and rlc compute queues */
	for (i = 0; i < sdma->num_instances; i++) {
		uint32_t temp;

		WREG32_SDMA(i, mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL, 0);
		sdma_v4_0_gfx_resume(adev, sdma, i);
		/* sdma_v4_0_page_resume: not ported */

		/* set utc l1 enable flag always to 1 */
		temp = RREG32_SDMA(i, mmSDMA0_CNTL);
		temp = REG_SET_FIELD(temp, SDMA0_CNTL, UTC_L1_ENABLE, 1);
		WREG32_SDMA(i, mmSDMA0_CNTL, temp);

		/* unhalt engine */
		temp = RREG32_SDMA(i, mmSDMA0_F32_CNTL);
		temp = REG_SET_FIELD(temp, SDMA0_F32_CNTL, HALT, 0);
		WREG32_SDMA(i, mmSDMA0_F32_CNTL, temp);
	}

	/* sdma_v4_0_rlc_resume -> sdma_v4_0_init_pg: no AMD_PG_SUPPORT_SDMA */

	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;

		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
		/* the page queue's test: not ported */
	}

	return r;
}

/* --- soft reset (amdgpu_device_gpu_recover's IP soft-reset arm; the file header) --- */

static struct frb_sdma4 *sdma_started(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = adev->sdma4;

	return sdma && sdma->touched && sdma->num_instances ? sdma : NULL;
}

/* sdma_v4_0_is_idle, per engine: one that is not idle counts as hung */
static int sdma_v4_0_check_soft_reset(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = sdma_started(adev);
	uint32_t hung = 0;
	int i;

	if (!sdma)
		return 0;
	for (i = 0; i < sdma->num_instances; i++)
		if (!(RREG32_SDMA(i, mmSDMA0_STATUS_REG) & SDMA0_STATUS_REG__IDLE_MASK))
			hung |= 1u << i;
	/*
	 * Both, as sdma_v3_0_check_soft_reset asks for both: pre_soft_reset
	 * and sdma_v4_0_start work on the pair.
	 */
	sdma->soft_reset = hung ? (1u << sdma->num_instances) - 1 : 0;
	return hung != 0;
}

/* not amdgpu: mark both engines hung (frbctl recover's forced levels) */
static void sdma_v4_0_force_soft_reset(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = sdma_started(adev);

	if (sdma)
		sdma->soft_reset = (1u << sdma->num_instances) - 1;
}

/* as sdma_v3_0_pre_soft_reset: the context switch off, the engines halted (their queues off first) */
static int sdma_v4_0_pre_soft_reset(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = sdma_started(adev);
	int i;

	if (!sdma || !sdma->soft_reset)
		return 0;
	for (i = 0; i < sdma->num_instances; i++)
		sdma->instance[i].ring.ready = 0;
	sdma_v4_0_ctx_switch_enable(adev, sdma, 0);
	sdma_v4_0_enable(adev, sdma, 0);
	return 0;
}

static int sdma_v4_0_soft_reset(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = sdma_started(adev);
	uint32_t tmp;
	int i;

	if (!sdma || !sdma->soft_reset)
		return 0;
	for (i = 0; i < sdma->num_instances; i++) {
		if (!(sdma->soft_reset & (1u << i)))
			continue;
		WREG32_SDMA(i, mmSDMA0_VIRT_RESET_REQ, SDMA0_VIRT_RESET_REQ__PF_MASK);
		tmp = RREG32_SDMA(i, mmSDMA0_VIRT_RESET_REQ);
		frb_info(adev, "SDMA%d_VIRT_RESET_REQ=0x%08X", i, tmp);

		udelay(50);

		WREG32_SDMA(i, mmSDMA0_VIRT_RESET_REQ, 0);
		tmp = RREG32_SDMA(i, mmSDMA0_VIRT_RESET_REQ);
	}
	/* Wait a little for things to settle down */
	udelay(50);
	return 0;
}

static void sdma_v4_0_update_medium_grain_clock_gating(struct frb_device *adev, struct frb_sdma4 *sdma,
						       int enable);
static void sdma_v4_0_update_medium_grain_light_sleep(struct frb_device *adev, struct frb_sdma4 *sdma,
						      int enable);

/*
 * The block is as a reset left it: hw_init again (the golden registers,
 * sdma_v4_0_start with its ring tests), then what the late init did (ULV,
 * the clock gating if the device is gated). The ring tests' result is
 * returned.
 */
static int sdma_v4_0_post_soft_reset(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = sdma_started(adev);
	int i, r;

	if (!sdma || !sdma->soft_reset)
		return 0;
	for (i = 0; i < sdma->num_instances; i++) {
		struct frb_ring *ring = &sdma->instance[i].ring;

		/* the engine's pointers restart at 0 (gfx_resume); so do the copies the driver reads */
		frb_wb_write64(adev, ring->rptr_offs, 0);
		frb_wb_write64(adev, ring->wptr_offs, 0);
	}
	sdma_v4_0_init_golden_registers(adev);
	r = sdma_v4_0_start(adev, sdma);
	if (adev->sdma_late_done)
		frb_sdma_v4_0_late_init(adev);
	if (adev->cg_gated) {
		sdma_v4_0_update_medium_grain_clock_gating(adev, sdma, 1);
		sdma_v4_0_update_medium_grain_light_sleep(adev, sdma, 1);
	}
	return r;
}

const struct frb_ip_reset_funcs frb_sdma_v4_0_reset_funcs = {
	.name = "sdma_v4_0",
	.check_soft_reset = sdma_v4_0_check_soft_reset,
	.force_soft_reset = sdma_v4_0_force_soft_reset,
	.pre_soft_reset = sdma_v4_0_pre_soft_reset,
	.soft_reset = sdma_v4_0_soft_reset,
	.post_soft_reset = sdma_v4_0_post_soft_reset,
};

/* sdma_v4_0_ring_pad_ib - pad the IB to the required number of dw */
static void sdma_v4_0_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	struct frb_sdma4_instance *sdma = amdgpu_sdma_get_instance_from_ring(ring);
	uint32_t pad_count;
	uint32_t i;

	pad_count = (-ib->length_dw) & 7;
	for (i = 0; i < pad_count; i++)
		if (sdma && sdma->burst_nop && (i == 0))
			frb_ib_write(ring->adev, ib,
				     SDMA_PKT_HEADER_OP(SDMA_OP_NOP) |
				     SDMA_PKT_NOP_HEADER_COUNT(pad_count - 1));
		else
			frb_ib_write(ring->adev, ib,
				     SDMA_PKT_HEADER_OP(SDMA_OP_NOP));
}

/* sdma_v4_0_ring_emit_pipeline_sync - wait for the ring's last fence */
static void sdma_v4_0_ring_emit_pipeline_sync(struct frb_ring *ring)
{
	uint32_t seq = ring->sync_seq;
	uint64_t addr = ring->fence_gpu_addr;

	/* wait for idle */
	sdma_v4_0_wait_reg_mem(ring, 1, 0,
			       addr & 0xfffffffc,
			       upper_32_bits(addr) & 0xffffffff,
			       seq, 0xffffffff, 4);
}

/* Another engine's fence: the pipeline sync's packet with the compare function given */
static void sdma_v4_0_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
{
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
		       SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(0) |
		       SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(1) |
		       SDMA_PKT_POLL_REGMEM_HEADER_FUNC(func));
	frb_ring_write(ring, lower_32_bits(addr) & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(addr));
	frb_ring_write(ring, ref);
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
		       SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(4));
}

/* sdma_v4_0_ring_emit_wreg (the HDP invalidate from the ring) */
static void sdma_v4_0_ring_emit_wreg(struct frb_ring *ring, uint32_t reg, uint32_t val)
{
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE) |
		       SDMA_PKT_SRBM_WRITE_HEADER_BYTE_EN(0xf));
	frb_ring_write(ring, reg);
	frb_ring_write(ring, val);
}

static const struct frb_ring_funcs sdma_v4_0_ring_funcs = {
	.align_mask = 0xff,
	.nop = SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP),
	.support_64bit_ptrs = 1,
	.get_rptr = sdma_v4_0_ring_get_rptr,
	.get_wptr = sdma_v4_0_ring_get_wptr,
	.set_wptr = sdma_v4_0_ring_set_wptr,
	.emit_ib = sdma_v4_0_ring_emit_ib,
	.emit_fence = sdma_v4_0_ring_emit_fence,
	.emit_hdp_flush = sdma_v4_0_ring_emit_hdp_flush,
	.insert_nop = sdma_v4_0_ring_insert_nop,
	.pad_ib = sdma_v4_0_ring_pad_ib,
	.emit_wreg = sdma_v4_0_ring_emit_wreg,
	.emit_pipeline_sync = sdma_v4_0_ring_emit_pipeline_sync,
	.emit_wait_mem = sdma_v4_0_ring_emit_wait_mem,
};

/* sdma_v4_0_emit_copy_buffer - byte_count bytes, GPU addresses (SDMA 4: the count less one) */
static void sdma_v4_0_emit_copy_buffer(struct frb_device *adev, struct frb_ib *ib, uint64_t src_offset,
				       uint64_t dst_offset, uint32_t byte_count, uint32_t copy_flags)
{
	(void)copy_flags;	/* no TMZ */
	frb_ib_write(adev, ib, SDMA_PKT_HEADER_OP(SDMA_OP_COPY) |
		     SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR) |
		     SDMA_PKT_COPY_LINEAR_HEADER_TMZ(0));
	frb_ib_write(adev, ib, byte_count - 1);
	frb_ib_write(adev, ib, 0); /* src/dst endian swap */
	frb_ib_write(adev, ib, lower_32_bits(src_offset));
	frb_ib_write(adev, ib, upper_32_bits(src_offset));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, upper_32_bits(dst_offset));
}

/*
 * sdma_v4_0_emit_fill_buffer - fill buffer using the sDMA engine (FILLSIZE
 * 0, as amdgpu; on SDMA v3 that repeats src_data's low byte, measured on the
 * RX 550; for SDMA v4 frbctl test sdmaN fill says on the card)
 */
static void sdma_v4_0_emit_fill_buffer(struct frb_device *adev, struct frb_ib *ib, uint32_t src_data,
				       uint64_t dst_offset, uint32_t byte_count)
{
	frb_ib_write(adev, ib, SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, upper_32_bits(dst_offset));
	frb_ib_write(adev, ib, src_data);
	frb_ib_write(adev, ib, byte_count - 1);
}

/* sdma_v4_0_buffer_funcs (not 4.4) */
#define SDMA_COPY_MAX_BYTES	(1u << 22)
#define SDMA_COPY_NUM_DW	7
#define SDMA_FILL_MAX_BYTES	(1u << 22)
#define SDMA_FILL_NUM_DW	5

static struct frb_ring *sdma_ring(struct frb_device *adev, int instance)
{
	struct frb_sdma4 *sdma = adev->sdma4;

	if (!sdma || instance < 0 || instance >= sdma->num_instances)
		return NULL;
	return &sdma->instance[instance].ring;
}

struct frb_ring *frb_sdma_v4_0_ring(struct frb_device *adev, int instance)
{
	struct frb_ring *ring = sdma_ring(adev, instance);

	return ring && ring->ready ? ring : NULL;
}

void frb_sdma_v4_0_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	sdma_v4_0_ring_pad_ib(ring, ib);
}

/* the job, submitted directly (amdgpu_job_submit_direct) and waited for: pad, schedule, poll */
static int sdma_submit_and_wait(struct frb_ring *ring, struct frb_ib *ib, const char *what)
{
	struct frb_device *adev = ring->adev;
	uint32_t seq = 0;
	int r;

	ring->funcs->pad_ib(ring, ib);
	if (ib->length_dw > ib->size_dw)
		return -ENOMEM;
	r = frb_ib_schedule(ring, ib, &seq);
	if (r)
		return r;
	r = frb_fence_wait(ring, seq, FRB_FENCE_TIMEOUT_US);
	if (r) {
		frb_err(adev, "%s: %s: fence %u not signalled (fence 0x%08x, rptr 0x%llx)", ring->name,
			what, seq, frb_fence_read(ring), (unsigned long long)ring->funcs->get_rptr(ring));
		return r;
	}
	frb_ib_free(adev, ib);
	return 0;
}

/* amdgpu_copy_buffer (amdgpu_ttm.c) on one SDMA ring, polled */
int frb_sdma_v4_0_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset,
			      uint64_t dst_offset, uint32_t byte_count)
{
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t max_bytes = SDMA_COPY_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready || !byte_count)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * SDMA_COPY_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size_in_bytes = byte_count < max_bytes ? byte_count : max_bytes;

		sdma_v4_0_emit_copy_buffer(adev, &ib, src_offset, dst_offset, cur_size_in_bytes, 0);
		src_offset += cur_size_in_bytes;
		dst_offset += cur_size_in_bytes;
		byte_count -= cur_size_in_bytes;
	}
	return sdma_submit_and_wait(ring, &ib, "copy");
}

/* amdgpu_ttm_fill_mem on one SDMA ring, polled */
int frb_sdma_v4_0_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data,
			      uint64_t dst_addr, uint32_t byte_count)
{
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t max_bytes = SDMA_FILL_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready || !byte_count)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * SDMA_FILL_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size = byte_count < max_bytes ? byte_count : max_bytes;

		sdma_v4_0_emit_fill_buffer(adev, &ib, src_data, dst_addr, cur_size);
		dst_addr += cur_size;
		byte_count -= cur_size;
	}
	return sdma_submit_and_wait(ring, &ib, "fill");
}

/* --- the copy and fill tests: VRAM to VRAM, checked by CPU readback -------- */

static uint32_t test_pattern(uint32_t seed, uint32_t i)
{
	uint32_t x = (i + 1) * 0x9e3779b1u ^ seed;

	x ^= x >> 15;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	return x;
}

/* dst[0, n) must be the pattern (or the fill value), dst[n, total) still the poison */
static int sdma_test_check(struct frb_device *adev, struct frb_ring *ring, const char *what,
			   uint32_t dst, uint32_t n, uint32_t total, uint32_t seed, int fill,
			   uint32_t fill_value)
{
	uint32_t i, got, want, bad = 0, first = 0, first_got = 0, first_want = 0;

	frb_invalidate_hdp(adev);
	for (i = 0; i < total; i++) {
		got = frb_vram_read32(adev, dst + 4 * i);
		want = i >= n ? FRB_SDMA_POISON : fill ? fill_value : test_pattern(seed, i);
		if (got != want) {
			if (!bad) {
				first = i;
				first_got = got;
				first_want = want;
			}
			bad++;
		}
	}
	if (bad) {
		frb_err(adev, "%s: %s: %u of %u dwords wrong; first at +0x%x: 0x%08x, want 0x%08x%s",
			ring->name, what, bad, total, first * 4, first_got, first_want,
			first >= n ? " (past the end: the guard)" : "");
		return -EIO;
	}
	return 0;
}

static int sdma_test_copy_fill(struct frb_device *adev, int instance, int fill)
{
	struct frb_sdma4 *sdma = adev->sdma4;
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t total = FRB_SDMA_TEST_BYTES / 4, bytes = FRB_SDMA_TEST_BYTES - FRB_SDMA_TEST_GUARD;
	uint32_t src, dst, seed, value, i;
	uint64_t t0;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	for (i = 0; i < 2; i++) {
		if (sdma->test_bo[i])
			continue;
		r = frb_vram_obj_alloc(adev, FRB_SDMA_TEST_BYTES, 4096, &sdma->test_bo[i]);
		if (r)
			return r;
	}
	src = sdma->test_bo[0];
	dst = sdma->test_bo[1];
	sdma->test_runs++;
	seed = 0x5d3a0000u ^ ((uint32_t)instance << 12) ^ (sdma->test_runs * 0x01000193u);
	/* four different bytes, the low one new each run: a byte fill repeats the low one */
	value = 0xa1b2c300u | ((0xd4u + sdma->test_runs * 0x1du) & 0xff);

	for (i = 0; i < total; i++) {
		if (!fill)
			frb_vram_write32(adev, src + 4 * i, test_pattern(seed, i));
		frb_vram_write32(adev, dst + 4 * i, FRB_SDMA_POISON);
	}
	/* the CPU's VRAM writes reach the engine through the HDP: frb_ring_commit flushes it */
	t0 = adev->os->time_ms(adev->os->cookie);
	if (fill)
		r = frb_sdma_v4_0_fill_buffer(adev, instance, value, frb_vram_gpu_addr(adev, dst), bytes);
	else
		r = frb_sdma_v4_0_copy_buffer(adev, instance, frb_vram_gpu_addr(adev, src),
					      frb_vram_gpu_addr(adev, dst), bytes);
	if (r)
		return r;
	t0 = adev->os->time_ms(adev->os->cookie) - t0;
	r = sdma_test_check(adev, ring, fill ? "fill" : "copy", dst, bytes / 4, total, seed, fill,
			    (value & 0xff) * 0x01010101u);
	if (r)
		return r;
	if (fill)
		frb_info(adev, "%s: fill test passed: src_data 0x%08x -> bytes 0x%02x over %u bytes at %llx, "
			 "%u-byte guard intact (%llu ms)", ring->name, value, value & 0xff, bytes,
			 (unsigned long long)frb_vram_gpu_addr(adev, dst), FRB_SDMA_TEST_GUARD,
			 (unsigned long long)t0);
	else
		frb_info(adev, "%s: copy test passed: %u bytes %llx -> %llx, %u-byte guard intact (%llu ms)",
			 ring->name, bytes, (unsigned long long)frb_vram_gpu_addr(adev, src),
			 (unsigned long long)frb_vram_gpu_addr(adev, dst), FRB_SDMA_TEST_GUARD,
			 (unsigned long long)t0);
	return 0;
}

/* --- the stage, the tests by hand, the teardown ------------------------------ */

int frb_sdma_v4_0_init(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = adev->sdma4;
	struct frb_ring *ring;
	char name[8];
	int r, i;

	if (adev->asic_type != CHIP_VEGA20) {
		frb_err(adev, "sdma: only the SDMA 4.2.0 (Vega 20) paths of sdma_v4_0 are ported");
		return -ENODEV;
	}
	if (adev->stage_done < FRB_STAGE_SMU || !adev->psp || !adev->aper)
		return -EPERM;
	if (!sdma) {
		sdma = (struct frb_sdma4 *)adev->os->alloc(adev->os->cookie, sizeof(*sdma));
		if (!sdma)
			return -ENOMEM;
		adev->sdma4 = sdma;
	}

	/* sdma_v4_0_early_init: two instances on Vega 20 */
	sdma->num_instances = SDMA_MAX_INSTANCE;
	r = sdma_v4_0_init_microcode(adev, sdma);
	if (r)
		return r;

	/* sdma_v4_0_sw_init: the gfx rings (no IRQ sources, no page rings) */
	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		if (ring->ring_size)
			continue;	/* a second stage run: the rings are there */
		name[0] = 's'; name[1] = 'd'; name[2] = 'm'; name[3] = 'a';
		name[4] = (char)('0' + i); name[5] = 0;
		r = frb_ring_init(adev, ring, name, &sdma_v4_0_ring_funcs, 1024);
		if (r)
			return r;
		ring->me = i;	/* sdma_v4_0_set_ring_funcs */
		ring->use_doorbell = frb_doorbell_usable(adev);
		/* doorbell size is 2 dwords, get DWORD offset */
		ring->doorbell_index = vega20_sdma_engine_doorbell[i] << 1;
		frb_wb_write64(adev, ring->wptr_offs, 0);
		frb_info(adev, "%s: microcode version %u, feature %u (%s NOPs); ring %u KB at %llx, WPTR by %s "
			 "(doorbell 0x%x)", ring->name, sdma->instance[i].fw_version,
			 sdma->instance[i].feature_version, sdma->instance[i].burst_nop ? "burst" : "single",
			 ring->ring_size >> 10, (unsigned long long)ring->gpu_addr,
			 ring->use_doorbell ? "doorbell" : "MMIO (GFX_RB_WPTR)", ring->doorbell_index);
	}

	/* sdma_v4_0_hw_init (not an APU) */
	sdma->touched = 1;
	sdma_v4_0_init_golden_registers(adev);

	r = sdma_v4_0_start(adev, sdma);
	if (r)
		return r;

	/* amdgpu_device_ip_late_init's amdgpu_ib_ring_tests, for these two rings */
	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		r = sdma_v4_0_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
		if (r) {
			frb_err(adev, "IB test failed on %s (%d).", ring->name, r);
			ring->ready = 0;
			return r;
		}
	}
	return 0;
}

int frb_sdma_v4_0_test(struct frb_device *adev, int instance, int test)
{
	struct frb_ring *ring = sdma_ring(adev, instance);

	if (!ring || !adev->sdma4->touched)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		return amdgpu_ring_test_helper(ring);
	case FRB_TEST_IB:
		if (!ring->ready)
			return -ENODEV;
		return sdma_v4_0_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_COPY:
		return sdma_test_copy_fill(adev, instance, 0);
	case FRB_TEST_FILL:
		return sdma_test_copy_fill(adev, instance, 1);
	default:
		return -EINVAL;
	}
}

/* sdma_v4_0_hw_fini: context switch off, both engines halted, before the driver lets go of VRAM */
void frb_sdma_v4_0_fini(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = adev->sdma4;
	int i;

	if (!sdma)
		return;
	for (i = 0; i < sdma->num_instances; i++)
		sdma->instance[i].ring.ready = 0;
	if (sdma->touched && sdma->num_instances) {
		sdma_v4_0_ctx_switch_enable(adev, sdma, 0);
		sdma_v4_0_enable(adev, sdma, 0);
		frb_info(adev, "sdma: both engines halted (hw_fini): SDMA0_F32_CNTL %08x, SDMA1_F32_CNTL %08x",
			 RREG32_SDMA(0, mmSDMA0_F32_CNTL), RREG32_SDMA(1, mmSDMA0_F32_CNTL));
	}
	adev->os->free(adev->os->cookie, sdma);
	adev->sdma4 = NULL;
}

/* --- late_init and clock gating -------------------------------------------------- */

/*
 * sdma_v4_0_setup_ulv (sdma_v4_0_late_init): the server SKU 0x66a1 takes
 * ULV hysteresis 0 (the RAS error counters it resets after are not ported)
 */
void frb_sdma_v4_0_late_init(struct frb_device *adev)
{
	struct frb_sdma4 *sdma = adev->sdma4;
	int i;

	if (!sdma || !sdma->touched || adev->device != 0x66a1)
		return;
	for (i = 0; i < sdma->num_instances; i++) {
		uint32_t temp;

		temp = RREG32_SDMA(i, mmSDMA0_ULV_CNTL);
		temp = REG_SET_FIELD(temp, SDMA0_ULV_CNTL, HYSTERESIS, 0x0);
		WREG32_SDMA(i, mmSDMA0_ULV_CNTL, temp);
	}
}

static void sdma_v4_0_update_medium_grain_clock_gating(struct frb_device *adev, struct frb_sdma4 *sdma,
						       int enable)
{
	uint32_t data, def;
	int i;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_SDMA_MGCG)) {
		for (i = 0; i < sdma->num_instances; i++) {
			def = data = RREG32_SDMA(i, mmSDMA0_CLK_CTRL);
			data &= ~(SDMA0_CLK_CTRL__SOFT_OVERRIDE7_MASK |
				  SDMA0_CLK_CTRL__SOFT_OVERRIDE6_MASK |
				  SDMA0_CLK_CTRL__SOFT_OVERRIDE5_MASK |
				  SDMA0_CLK_CTRL__SOFT_OVERRIDE4_MASK |
				  SDMA0_CLK_CTRL__SOFT_OVERRIDE3_MASK |
				  SDMA0_CLK_CTRL__SOFT_OVERRIDE2_MASK |
				  SDMA0_CLK_CTRL__SOFT_OVERRIDE1_MASK |
				  SDMA0_CLK_CTRL__SOFT_OVERRIDE0_MASK);
			if (def != data)
				WREG32_SDMA(i, mmSDMA0_CLK_CTRL, data);
		}
	} else {
		for (i = 0; i < sdma->num_instances; i++) {
			def = data = RREG32_SDMA(i, mmSDMA0_CLK_CTRL);
			data |= (SDMA0_CLK_CTRL__SOFT_OVERRIDE7_MASK |
				 SDMA0_CLK_CTRL__SOFT_OVERRIDE6_MASK |
				 SDMA0_CLK_CTRL__SOFT_OVERRIDE5_MASK |
				 SDMA0_CLK_CTRL__SOFT_OVERRIDE4_MASK |
				 SDMA0_CLK_CTRL__SOFT_OVERRIDE3_MASK |
				 SDMA0_CLK_CTRL__SOFT_OVERRIDE2_MASK |
				 SDMA0_CLK_CTRL__SOFT_OVERRIDE1_MASK |
				 SDMA0_CLK_CTRL__SOFT_OVERRIDE0_MASK);
			if (def != data)
				WREG32_SDMA(i, mmSDMA0_CLK_CTRL, data);
		}
	}
}

/*
 * sdma_v4_0_update_medium_grain_light_sleep. Linux's loop reads and writes
 * instance 0's SDMA0_POWER_CNTL each time round (RREG32_SDMA(0, ...)), so
 * SDMA1's memory light sleep is never touched; kept as Linux has it (the
 * capture shows SDMA0's twice, SDMA1's never).
 */
static void sdma_v4_0_update_medium_grain_light_sleep(struct frb_device *adev, struct frb_sdma4 *sdma,
						      int enable)
{
	uint32_t data, def;
	int i;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_SDMA_LS)) {
		for (i = 0; i < sdma->num_instances; i++) {
			/* 1-not override: enable sdma mem light sleep */
			def = data = RREG32_SDMA(0, mmSDMA0_POWER_CNTL);
			data |= SDMA0_POWER_CNTL__MEM_POWER_OVERRIDE_MASK;
			if (def != data)
				WREG32_SDMA(0, mmSDMA0_POWER_CNTL, data);
		}
	} else {
		for (i = 0; i < sdma->num_instances; i++) {
			/* 0-override:disable sdma mem light sleep */
			def = data = RREG32_SDMA(0, mmSDMA0_POWER_CNTL);
			data &= ~SDMA0_POWER_CNTL__MEM_POWER_OVERRIDE_MASK;
			if (def != data)
				WREG32_SDMA(0, mmSDMA0_POWER_CNTL, data);
		}
	}
}

/* sdma_v4_0_set_clockgating_state (bare metal); set_powergating_state is 4.1.x only */
void frb_sdma_v4_0_set_clockgating_state(struct frb_device *adev, int gate)
{
	struct frb_sdma4 *sdma = adev->sdma4;

	if (!frb_sdma_v4_0_up(adev))
		return;
	sdma_v4_0_update_medium_grain_clock_gating(adev, sdma, gate);
	sdma_v4_0_update_medium_grain_light_sleep(adev, sdma, gate);
}

int frb_sdma_v4_0_up(struct frb_device *adev)
{
	return adev->sdma4 && adev->sdma4->touched && adev->sdma4->num_instances;
}

/*
 * The engines' state in one line, for the log: STATUS_REG (bit 0 idle),
 * UTCL1_WR_STATUS (503001ff at rest; RPTR_DATA_FIFO_EMPTY, bit 28, clear with
 * a merge state is the stall of docs/vega20-bring-up.md), GFX_RB_RPTR_ADDR_LO's
 * RPTR_WB_IDLE, and the queue's pointers.
 */
void frb_sdma_v4_0_log_state(struct frb_device *adev, const char *where)
{
	struct frb_sdma4 *sdma = adev->sdma4;
	uint32_t st[SDMA_MAX_INSTANCE], wr[SDMA_MAX_INSTANCE], wb[SDMA_MAX_INSTANCE], rp[SDMA_MAX_INSTANCE],
		 wp[SDMA_MAX_INSTANCE], ulv[SDMA_MAX_INSTANCE], ctx[SDMA_MAX_INSTANCE];
	int i;

	if (!sdma || !sdma->touched || sdma->num_instances != SDMA_MAX_INSTANCE)
		return;
	for (i = 0; i < SDMA_MAX_INSTANCE; i++) {
		st[i] = RREG32_SDMA(i, mmSDMA0_STATUS_REG);
		wr[i] = RREG32_SDMA(i, mmSDMA0_UTCL1_WR_STATUS);
		wb[i] = RREG32_SDMA(i, mmSDMA0_GFX_RB_RPTR_ADDR_LO) & 1;
		rp[i] = RREG32_SDMA(i, mmSDMA0_GFX_RB_RPTR);
		wp[i] = RREG32_SDMA(i, mmSDMA0_GFX_RB_WPTR);
		ulv[i] = RREG32_SDMA(i, mmSDMA0_ULV_CNTL);
		ctx[i] = RREG32_SDMA(i, mmSDMA0_GFX_CONTEXT_STATUS);
	}
	frb_info(adev, "sdma state (%s, %llu ms): sdma0 STATUS %08x UTCL1_WR %08x wb_idle %u rptr %x wptr %x ULV %08x ctx %x; "
		 "sdma1 STATUS %08x UTCL1_WR %08x wb_idle %u rptr %x wptr %x ULV %08x ctx %x", where,
		 (unsigned long long)adev->os->time_ms(adev->os->cookie), st[0], wr[0], wb[0], rp[0], wp[0], ulv[0], ctx[0], st[1], wr[1], wb[1], rp[1], wp[1], ulv[1], ctx[1]);
}
