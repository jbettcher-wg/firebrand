/*
 * sdma_v3_0.c (Linux 7.0.2), for Polaris 12: the two SDMA engines' gfx
 * rings. Stage sdma runs what amdgpu does for this block at load:
 * early_init (num_instances, the firmware headers: burst NOP), sw_init (the
 * two rings), hw_init (golden registers, sdma_v3_0_start: halt, gfx_resume,
 * unhalt, context switch, the ring tests), then the IB tests amdgpu runs from
 * late_init (amdgpu_ib_ring_tests). hw_fini halts both engines at unload.
 * The microcode is in the engines already: the SMU loaded SDMA0/SDMA1 from
 * its table of contents in stage smu, as on Linux for Polaris (no
 * sdma_v3_0_load_microcode).
 *
 * Also here: amdgpu_ttm's copy and fill for this engine (amdgpu_copy_buffer,
 * amdgpu_ttm_fill_mem on sdma_v3_0_emit_copy_buffer / _emit_fill_buffer),
 * polled; they take GPU (MC) addresses, so they work for VRAM<->GTT once a
 * GTT exists. frbctl `test sdma0|sdma1 ring|ib|copy|fill` runs the tests.
 *
 * DEVIATIONS from amdgpu, each with its register-level consequence:
 * - PLACEMENT. Rings, writeback and IBs live in VRAM (frb_ring.c's objects
 *   region) until GTT exists; Linux keeps them in GTT. So RB_BASE,
 *   RB_RPTR_ADDR_HI/LO and RB_WPTR_POLL_ADDR_HI/LO carry other addresses.
 * - NO DOORBELLS. Linux sets ring->use_doorbell on VI (sdma_v3_0_sw_init):
 *   SDMA0_GFX_DOORBELL gets OFFSET 0x1e0/0x1e1 and ENABLE, and set_wptr
 *   rings the doorbell. Here use_doorbell is false, Linux's own other arm:
 *   DOORBELL ENABLE=0 and the WPTR goes to SDMA0_GFX_RB_WPTR by MMIO
 *   (writes the Linux trace does not have).
 * - NO INTERRUPTS. SDMA0_CNTL.TRAP_ENABLE is never set (Linux sets it from
 *   late_init through amdgpu_irq_get); the TRAP packets after each fence
 *   raise nothing, and fences are polled from writeback.
 * - BYTE ORDER. Under __BIG_ENDIAN Linux would set RB_SWAP_ENABLE,
 *   RPTR_WRITEBACK_SWAP_ENABLE and IB_SWAP_ENABLE. They stay off: the ring,
 *   the IBs and the writeback are little-endian dwords written and read
 *   with explicit LE accessors (frb_ring_write, frb_ib_write, frb_wb_read),
 *   so every register value is the little-endian Linux capture's
 *   (docs/byte-order.md).
 * - SDMA0_TILING_CONFIG takes gfx.config.gb_addr_config & 0x70; GFX's
 *   gpu_early_init is not in this branch, so the value is computed here the
 *   same way (sdma_v3_0_gb_addr_config).
 * - The IB tests run at the end of the stage, not after every other block's
 *   hw_init as amdgpu_device_ip_late_init does; they touch no registers but
 *   the HDP flush/invalidate and the WPTR.
 * - Not ported: doorbells, interrupts/IH (trap and illegal-instruction
 *   sources), the RLC compute queues (Linux's sdma_v3_0_rlc_resume is
 *   empty too), page/vm-pte functions, soft reset, clock/power gating (a
 *   no-op for Polaris 12 in Linux), SR-IOV pollmem.
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
 *
 * Authors: Alex Deucher
 */
#include "frb_ring.h"
#include "frb_ucode.h"
#include "include/asic_reg/oss/oss_3_0_d.h"
#include "include/asic_reg/oss/oss_3_0_sh_mask.h"
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/bif/bif_5_0_sh_mask.h"
#include "include/tonga_sdma_pkt_open.h"

/* vid.h */
#define SDMA0_REGISTER_OFFSET	0x0	/* not a register */
#define SDMA1_REGISTER_OFFSET	0x200	/* not a register */
#define SDMA_MAX_INSTANCE	2

/* gfx_v8_0.c */
#define POLARIS11_GB_ADDR_CONFIG_GOLDEN	0x22011002

/* amdgpu_drv.c module parameter default */
static const uint32_t amdgpu_sdma_phase_quantum = 32;

/* the copy/fill tests: two buffers in the VRAM objects region */
#define FRB_SDMA_TEST_BYTES	(256u << 10)
#define FRB_SDMA_TEST_GUARD	256u	/* bytes left untouched at the end of the target */
#define FRB_SDMA_POISON		0xdeadd00du

/* amdgpu_sdma.h amdgpu_sdma_instance, the fields this port uses */
struct frb_sdma_instance {
	struct frb_ring ring;
	uint32_t fw_version;
	uint32_t feature_version;
	int burst_nop;
};

/* amdgpu_sdma.h amdgpu_sdma */
struct frb_sdma {
	struct frb_sdma_instance instance[SDMA_MAX_INSTANCE];
	int num_instances;
	int touched;			/* hw_init wrote the engines: hw_fini must halt them */
	uint32_t test_bo[2];		/* BAR0 offsets of the copy/fill buffers, 0 = none yet */
	uint32_t test_runs;
	uint32_t srbm_soft_reset;	/* check_soft_reset's verdict */
};

static const uint32_t sdma_offsets[SDMA_MAX_INSTANCE] =
{
	SDMA0_REGISTER_OFFSET,
	SDMA1_REGISTER_OFFSET
};

static const uint32_t golden_settings_polaris10_a11[] =
{
	mmSDMA0_CHICKEN_BITS, 0xfc910007, 0x00810007,
	mmSDMA0_CLK_CTRL, 0xff000fff, 0x00000000,
	mmSDMA0_GFX_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA0_RLC0_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA0_RLC1_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA1_CHICKEN_BITS, 0xfc910007, 0x00810007,
	mmSDMA1_CLK_CTRL, 0xff000fff, 0x00000000,
	mmSDMA1_GFX_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA1_RLC0_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA1_RLC1_IB_CNTL, 0x800f0111, 0x00000100,
};

static const uint32_t golden_settings_polaris11_a11[] =
{
	mmSDMA0_CHICKEN_BITS, 0xfc910007, 0x00810007,
	mmSDMA0_CLK_CTRL, 0xff000fff, 0x00000000,
	mmSDMA0_GFX_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA0_RLC0_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA0_RLC1_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA1_CHICKEN_BITS, 0xfc910007, 0x00810007,
	mmSDMA1_CLK_CTRL, 0xff000fff, 0x00000000,
	mmSDMA1_GFX_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA1_RLC0_IB_CNTL, 0x800f0111, 0x00000100,
	mmSDMA1_RLC1_IB_CNTL, 0x800f0111, 0x00000100,
};

static inline uint32_t lower_32_bits(uint64_t v) { return (uint32_t)v; }
static inline uint32_t upper_32_bits(uint64_t v) { return (uint32_t)(v >> 32); }

/* include/linux/log2.h order_base_2: log2, rounded up */
static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

/*
 * vi.c vi_srbm_select, a local copy until frb_vi.c exports one (gfx_v8_0
 * needs it too). The caller holds the driver's lock (Linux: srbm_mutex).
 */
static void vi_srbm_select(struct frb_device *adev,
			   uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid)
{
	uint32_t srbm_gfx_cntl = 0;

	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, PIPEID, pipe);
	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, MEID, me);
	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, VMID, vmid);
	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, QUEUEID, queue);
	WREG32(mmSRBM_GFX_CNTL, srbm_gfx_cntl);
}

/*
 * adev->gfx.config.gb_addr_config as gfx_v8_0_gpu_early_init sets it for
 * Polaris 11/12: the golden value with ROW_SIZE from MC_ARB_RAMCFG. Only
 * bits 6:4 (PIPE_INTERLEAVE_SIZE) reach SDMA, and ROW_SIZE is not among
 * them, so the golden value is the answer (the E9173's debugfs gca_config
 * says 0x22011002). When frb_gfx_v8_0.c has gpu_early_init, read it there.
 */
static uint32_t sdma_v3_0_gb_addr_config(struct frb_device *adev)
{
	/* Polaris 10 is Tonga's value (0x22011003): the same bits 6:4 */
	return adev->asic_type == CHIP_POLARIS10 ? 0x22011003 : POLARIS11_GB_ADDR_CONFIG_GOLDEN;
}

static void sdma_v3_0_init_golden_registers(struct frb_device *adev)
{
	switch (adev->asic_type) {
	case CHIP_POLARIS10:
		frb_device_program_register_sequence(adev,
						     golden_settings_polaris10_a11,
						     ARRAY_SIZE(golden_settings_polaris10_a11));
		break;
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
	case CHIP_VEGAM:
		frb_device_program_register_sequence(adev,
						     golden_settings_polaris11_a11,
						     ARRAY_SIZE(golden_settings_polaris11_a11));
		break;
	default:
		break;
	}
}

/*
 * sdma_v3_0_init_microcode, the header half: the SMU loads the images
 * (stage smu); the driver keeps the version and the feature level, which
 * decides burst NOPs (SDMA0's file says 31, SDMA1's 0: only SDMA0 bursts,
 * as on Linux).
 */
static int sdma_v3_0_init_microcode(struct frb_device *adev, struct frb_sdma *sdma)
{
	const char *chip_name;
	char name[32];
	struct frb_fw fw;
	struct frb_ucode u;
	int err, i;

	switch (adev->asic_type) {
	case CHIP_POLARIS12:
		chip_name = "polaris12";
		break;
	case CHIP_POLARIS10:
		chip_name = "polaris10";
		break;
	default:
		frb_err(adev, "sdma: only Polaris 12 and Polaris 10 are wired up");
		return -ENODEV;
	}

	for (i = 0; i < sdma->num_instances; i++) {
		frb_fw_name(name, sizeof(name), chip_name, i == 0 ? "_sdma.bin" : "_sdma1.bin");
		fw.data = 0;
		fw.size = 0;
		err = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
		if (err) {
			frb_err(adev, "sdma_v3_0: Failed to load firmware \"%s\" (%d)", name, err);
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

static struct frb_sdma_instance *amdgpu_sdma_get_instance_from_ring(struct frb_ring *ring)
{
	struct frb_sdma *sdma = ring->adev->sdma;

	if (!sdma || ring->me < 0 || ring->me >= sdma->num_instances)
		return NULL;
	return &sdma->instance[ring->me];
}

/*
 * sdma_v3_0_ring_get_rptr - get the current read pointer, from writeback.
 * (Linux's "XXX check if swapping is necessary on BE": the writeback dword
 * is little-endian; frb_wb_read decodes it.)
 */
static uint64_t sdma_v3_0_ring_get_rptr(struct frb_ring *ring)
{
	return frb_wb_read(ring->adev, ring->rptr_offs) >> 2;
}

/* sdma_v3_0_ring_get_wptr - no doorbell, no pollmem: the register */
static uint64_t sdma_v3_0_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t wptr;

	wptr = RREG32(mmSDMA0_GFX_RB_WPTR + sdma_offsets[ring->me]) >> 2;

	return wptr;
}

/* sdma_v3_0_ring_set_wptr - no doorbell, no pollmem: the register */
static void sdma_v3_0_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmSDMA0_GFX_RB_WPTR + sdma_offsets[ring->me], (uint32_t)(ring->wptr << 2));
}

static void sdma_v3_0_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	struct frb_sdma_instance *sdma = amdgpu_sdma_get_instance_from_ring(ring);
	uint32_t i;

	for (i = 0; i < count; i++)
		if (sdma && sdma->burst_nop && (i == 0))
			frb_ring_write(ring, ring->funcs->nop |
				       SDMA_PKT_NOP_HEADER_COUNT(count - 1));
		else
			frb_ring_write(ring, ring->funcs->nop);
}

/* sdma_v3_0_ring_emit_ib - Schedule an IB on the DMA engine (vmid: 0 for kernel IBs) */
static void sdma_v3_0_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib,
				   uint32_t vmid, uint32_t flags)
{
	(void)flags;

	/* IB packet must end on a 8 DW boundary */
	sdma_v3_0_ring_insert_nop(ring, (2 - lower_32_bits(ring->wptr)) & 7);

	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT) |
		       SDMA_PKT_INDIRECT_HEADER_VMID(vmid & 0xf));
	/* base must be 32 byte aligned */
	frb_ring_write(ring, lower_32_bits(ib->gpu_addr) & 0xffffffe0);
	frb_ring_write(ring, upper_32_bits(ib->gpu_addr));
	frb_ring_write(ring, ib->length_dw);
	frb_ring_write(ring, 0);
	frb_ring_write(ring, 0);
}

/* sdma_v3_0_ring_emit_hdp_flush - emit an hdp flush on the DMA ring */
static void sdma_v3_0_ring_emit_hdp_flush(struct frb_ring *ring)
{
	uint32_t ref_and_mask = 0;

	if (ring->me == 0)
		ref_and_mask = REG_SET_FIELD(ref_and_mask, GPU_HDP_FLUSH_DONE, SDMA0, 1);
	else
		ref_and_mask = REG_SET_FIELD(ref_and_mask, GPU_HDP_FLUSH_DONE, SDMA1, 1);

	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
		       SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(1) |
		       SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3)); /* == */
	frb_ring_write(ring, mmGPU_HDP_FLUSH_DONE << 2);
	frb_ring_write(ring, mmGPU_HDP_FLUSH_REQ << 2);
	frb_ring_write(ring, ref_and_mask); /* reference */
	frb_ring_write(ring, ref_and_mask); /* mask */
	frb_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
		       SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(10)); /* retry count, poll interval */
}

/*
 * sdma_v3_0_ring_emit_fence - emit a fence on the DMA ring (the trap that
 * follows raises nothing: TRAP_ENABLE is off, fences are polled)
 */
static void sdma_v3_0_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq,
				      unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;

	/* write the fence */
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE));
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

/* sdma_v3_0_gfx_stop - stop the gfx async dma engines */
static void sdma_v3_0_gfx_stop(struct frb_device *adev, struct frb_sdma *sdma)
{
	uint32_t rb_cntl, ib_cntl;
	int i;

	for (i = 0; i < sdma->num_instances; i++) {
		rb_cntl = RREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i]);
		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 0);
		WREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i], rb_cntl);
		ib_cntl = RREG32(mmSDMA0_GFX_IB_CNTL + sdma_offsets[i]);
		ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 0);
		WREG32(mmSDMA0_GFX_IB_CNTL + sdma_offsets[i], ib_cntl);
		sdma->instance[i].ring.ready = 0;
	}
}

/* sdma_v3_0_rlc_stop - stop the compute async dma engines (empty in Linux too) */
static void sdma_v3_0_rlc_stop(struct frb_device *adev)
{
	/* XXX todo */
	(void)adev;
}

/* sdma_v3_0_ctx_switch_enable - the async dma engines' context switch on (1) or off (0) */
static void sdma_v3_0_ctx_switch_enable(struct frb_device *adev, struct frb_sdma *sdma, int enable)
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
			frb_info(adev, "clamping sdma_phase_quantum to %uK clock cycles", value << unit);
		}
		phase_quantum =
			value << SDMA0_PHASE0_QUANTUM__VALUE__SHIFT |
			unit  << SDMA0_PHASE0_QUANTUM__UNIT__SHIFT;
	}

	for (i = 0; i < sdma->num_instances; i++) {
		f32_cntl = RREG32(mmSDMA0_CNTL + sdma_offsets[i]);
		if (enable) {
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL,
					AUTO_CTXSW_ENABLE, 1);
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL,
					ATC_L1_ENABLE, 1);
			if (amdgpu_sdma_phase_quantum) {
				WREG32(mmSDMA0_PHASE0_QUANTUM + sdma_offsets[i],
				       phase_quantum);
				WREG32(mmSDMA0_PHASE1_QUANTUM + sdma_offsets[i],
				       phase_quantum);
			}
		} else {
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL,
					AUTO_CTXSW_ENABLE, 0);
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL,
					ATC_L1_ENABLE, 1);
		}

		WREG32(mmSDMA0_CNTL + sdma_offsets[i], f32_cntl);
	}
}

/* sdma_v3_0_enable - halt (0) or unhalt (1) the async dma engines */
static void sdma_v3_0_enable(struct frb_device *adev, struct frb_sdma *sdma, int enable)
{
	uint32_t f32_cntl;
	int i;

	if (!enable) {
		sdma_v3_0_gfx_stop(adev, sdma);
		sdma_v3_0_rlc_stop(adev);
	}

	for (i = 0; i < sdma->num_instances; i++) {
		f32_cntl = RREG32(mmSDMA0_F32_CNTL + sdma_offsets[i]);
		if (enable)
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, 0);
		else
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, 1);
		WREG32(mmSDMA0_F32_CNTL + sdma_offsets[i], f32_cntl);
	}
}

/*
 * sdma_v3_0_ring_test_ring - simple async dma engine test: the engine writes
 * a value to a writeback dword (VRAM here, read back after an HDP
 * invalidate by frb_wb_read)
 */
static int sdma_v3_0_ring_test_ring(struct frb_ring *ring)
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
	frb_ring_write(ring, SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(1));
	frb_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = frb_wb_read(adev, index);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout) {
		frb_err(adev, "%s: ring test: the value is still 0x%08x after %u polls (rptr 0x%llx, wptr 0x%llx)",
			ring->name, tmp, i, (unsigned long long)ring->funcs->get_rptr(ring),
			(unsigned long long)ring->funcs->get_wptr(ring));
		r = -ETIMEDOUT;
	} else {
		frb_info(adev, "%s: ring test passed after %u polls (rptr 0x%llx)", ring->name, i,
			 (unsigned long long)ring->funcs->get_rptr(ring));
	}

error_free_wb:
	frb_wb_free(adev, index);
	return r;
}

/* sdma_v3_0_ring_test_ib - test an IB on the DMA engine (the fence polled) */
static int sdma_v3_0_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
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
	frb_ib_write(adev, &ib, SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(1));
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

	r = sdma_v3_0_ring_test_ring(ring);
	if (r)
		frb_err(adev, "ring %s test failed (%d)", ring->name, r);
	else
		frb_dbg(adev, "ring test on %s succeeded", ring->name);

	ring->ready = !r;

	return r;
}

/* sdma_v3_0_gfx_resume - setup and start the async dma engines */
static int sdma_v3_0_gfx_resume(struct frb_device *adev, struct frb_sdma *sdma)
{
	struct frb_ring *ring;
	uint32_t rb_cntl, ib_cntl, wptr_poll_cntl;
	uint32_t rb_bufsz;
	uint32_t doorbell;
	uint64_t wptr_gpu_addr;
	int i, j, r;

	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		frb_ring_clear(ring);

		for (j = 0; j < 16; j++) {
			vi_srbm_select(adev, 0, 0, 0, j);
			/* SDMA GFX */
			WREG32(mmSDMA0_GFX_VIRTUAL_ADDR + sdma_offsets[i], 0);
			WREG32(mmSDMA0_GFX_APE1_CNTL + sdma_offsets[i], 0);
		}
		vi_srbm_select(adev, 0, 0, 0, 0);

		WREG32(mmSDMA0_TILING_CONFIG + sdma_offsets[i],
		       sdma_v3_0_gb_addr_config(adev) & 0x70);

		WREG32(mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL + sdma_offsets[i], 0);

		/* Set ring buffer size in dwords */
		rb_bufsz = order_base_2(ring->ring_size / 4);
		rb_cntl = RREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i]);
		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_SIZE, rb_bufsz);
		/*
		 * Linux sets RB_SWAP_ENABLE and RPTR_WRITEBACK_SWAP_ENABLE here
		 * under __BIG_ENDIAN; the ring and the writeback are little-endian.
		 */
		WREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i], rb_cntl);

		/* Initialize the ring buffer's read and write pointers */
		ring->wptr = 0;
		WREG32(mmSDMA0_GFX_RB_RPTR + sdma_offsets[i], 0);
		sdma_v3_0_ring_set_wptr(ring);
		WREG32(mmSDMA0_GFX_IB_RPTR + sdma_offsets[i], 0);
		WREG32(mmSDMA0_GFX_IB_OFFSET + sdma_offsets[i], 0);

		/* set the wb address whether it's enabled or not */
		WREG32(mmSDMA0_GFX_RB_RPTR_ADDR_HI + sdma_offsets[i],
		       upper_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFF);
		WREG32(mmSDMA0_GFX_RB_RPTR_ADDR_LO + sdma_offsets[i],
		       lower_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFC);

		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);

		WREG32(mmSDMA0_GFX_RB_BASE + sdma_offsets[i], (uint32_t)(ring->gpu_addr >> 8));
		WREG32(mmSDMA0_GFX_RB_BASE_HI + sdma_offsets[i], (uint32_t)(ring->gpu_addr >> 40));

		doorbell = RREG32(mmSDMA0_GFX_DOORBELL + sdma_offsets[i]);

		/* use_doorbell is false (no doorbells yet): Linux's else arm */
		doorbell = REG_SET_FIELD(doorbell, SDMA0_GFX_DOORBELL, ENABLE, 0);
		WREG32(mmSDMA0_GFX_DOORBELL + sdma_offsets[i], doorbell);

		/* setup the wptr shadow polling */
		wptr_gpu_addr = ring->wptr_gpu_addr;

		WREG32(mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO + sdma_offsets[i],
		       lower_32_bits(wptr_gpu_addr));
		WREG32(mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI + sdma_offsets[i],
		       upper_32_bits(wptr_gpu_addr));
		wptr_poll_cntl = RREG32(mmSDMA0_GFX_RB_WPTR_POLL_CNTL + sdma_offsets[i]);
		/* use_pollmem is SR-IOV only: the else arm */
		wptr_poll_cntl = REG_SET_FIELD(wptr_poll_cntl,
					       SDMA0_GFX_RB_WPTR_POLL_CNTL,
					       ENABLE, 0);
		WREG32(mmSDMA0_GFX_RB_WPTR_POLL_CNTL + sdma_offsets[i], wptr_poll_cntl);

		/* enable DMA RB */
		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 1);
		WREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i], rb_cntl);

		ib_cntl = RREG32(mmSDMA0_GFX_IB_CNTL + sdma_offsets[i]);
		ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 1);
		/* IB_SWAP_ENABLE (Linux, __BIG_ENDIAN) stays off: IBs are little-endian */
		/* enable DMA IBs */
		WREG32(mmSDMA0_GFX_IB_CNTL + sdma_offsets[i], ib_cntl);
	}

	/* unhalt the MEs */
	sdma_v3_0_enable(adev, sdma, 1);
	/* enable sdma ring preemption */
	sdma_v3_0_ctx_switch_enable(adev, sdma, 1);

	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	return 0;
}

/* sdma_v3_0_rlc_resume - the compute queues (empty in Linux too) */
static int sdma_v3_0_rlc_resume(struct frb_device *adev)
{
	(void)adev;
	return 0;
}

/* sdma_v3_0_start - setup and start the async dma engines */
static int sdma_v3_0_start(struct frb_device *adev, struct frb_sdma *sdma)
{
	int r;

	/* disable sdma engine before programing it */
	sdma_v3_0_ctx_switch_enable(adev, sdma, 0);
	sdma_v3_0_enable(adev, sdma, 0);

	/* start the gfx rings and rlc compute queues */
	r = sdma_v3_0_gfx_resume(adev, sdma);
	if (r)
		return r;
	r = sdma_v3_0_rlc_resume(adev);
	if (r)
		return r;

	return 0;
}

/* --- soft reset (amdgpu_device_gpu_recover's IP soft-reset arm) -------------- */

static struct frb_sdma *sdma_started(struct frb_device *adev)
{
	struct frb_sdma *sdma = adev->sdma;

	return sdma && sdma->touched ? sdma : NULL;
}

static int sdma_v3_0_check_soft_reset(struct frb_device *adev)
{
	struct frb_sdma *sdma = sdma_started(adev);
	uint32_t srbm_soft_reset = 0;
	uint32_t tmp;

	if (!sdma)
		return 0;
	tmp = RREG32(mmSRBM_STATUS2);
	if ((tmp & SRBM_STATUS2__SDMA_BUSY_MASK) ||
	    (tmp & SRBM_STATUS2__SDMA1_BUSY_MASK)) {
		srbm_soft_reset |= SRBM_SOFT_RESET__SOFT_RESET_SDMA_MASK;
		srbm_soft_reset |= SRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK;
	}

	sdma->srbm_soft_reset = srbm_soft_reset;
	return srbm_soft_reset != 0;
}

/* not amdgpu: mark both engines hung (frbctl recover's forced levels) */
static void sdma_v3_0_force_soft_reset(struct frb_device *adev)
{
	struct frb_sdma *sdma = sdma_started(adev);

	if (sdma)
		sdma->srbm_soft_reset = SRBM_SOFT_RESET__SOFT_RESET_SDMA_MASK |
					SRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK;
}

static int sdma_v3_0_pre_soft_reset(struct frb_device *adev)
{
	struct frb_sdma *sdma = sdma_started(adev);
	uint32_t srbm_soft_reset = 0;

	if (!sdma || !sdma->srbm_soft_reset)
		return 0;

	srbm_soft_reset = sdma->srbm_soft_reset;

	if (REG_GET_FIELD(srbm_soft_reset, SRBM_SOFT_RESET, SOFT_RESET_SDMA) ||
	    REG_GET_FIELD(srbm_soft_reset, SRBM_SOFT_RESET, SOFT_RESET_SDMA1)) {
		sdma_v3_0_ctx_switch_enable(adev, sdma, 0);
		sdma_v3_0_enable(adev, sdma, 0);
	}

	return 0;
}

/* sdma_v3_0_post_soft_reset; gfx_resume's ring tests' result is returned */
static int sdma_v3_0_post_soft_reset(struct frb_device *adev)
{
	struct frb_sdma *sdma = sdma_started(adev);
	uint32_t srbm_soft_reset = 0;
	int r = 0;

	if (!sdma || !sdma->srbm_soft_reset)
		return 0;

	srbm_soft_reset = sdma->srbm_soft_reset;

	if (REG_GET_FIELD(srbm_soft_reset, SRBM_SOFT_RESET, SOFT_RESET_SDMA) ||
	    REG_GET_FIELD(srbm_soft_reset, SRBM_SOFT_RESET, SOFT_RESET_SDMA1)) {
		r = sdma_v3_0_gfx_resume(adev, sdma);
		sdma_v3_0_rlc_resume(adev);
	}

	return r;
}

static int sdma_v3_0_soft_reset(struct frb_device *adev)
{
	struct frb_sdma *sdma = sdma_started(adev);
	uint32_t srbm_soft_reset = 0;
	uint32_t tmp;

	if (!sdma || !sdma->srbm_soft_reset)
		return 0;

	srbm_soft_reset = sdma->srbm_soft_reset;

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

		/* Wait a little for things to settle down */
		udelay(50);
	}

	return 0;
}

const struct frb_ip_reset_funcs frb_sdma_v3_0_reset_funcs = {
	.name = "sdma_v3_0",
	.check_soft_reset = sdma_v3_0_check_soft_reset,
	.force_soft_reset = sdma_v3_0_force_soft_reset,
	.pre_soft_reset = sdma_v3_0_pre_soft_reset,
	.soft_reset = sdma_v3_0_soft_reset,
	.post_soft_reset = sdma_v3_0_post_soft_reset,
};

/* sdma_v3_0_ring_pad_ib - pad the IB to the required number of dw */
static void sdma_v3_0_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	struct frb_sdma_instance *sdma = amdgpu_sdma_get_instance_from_ring(ring);
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

/* sdma_v3_0_ring_emit_wreg (vi_flush_hdp / vi_invalidate_hdp from the ring) */
static void sdma_v3_0_ring_emit_wreg(struct frb_ring *ring,
				     uint32_t reg, uint32_t val)
{
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE) |
		       SDMA_PKT_SRBM_WRITE_HEADER_BYTE_EN(0xf));
	frb_ring_write(ring, reg);
	frb_ring_write(ring, val);
}

/*
 * sdma_v3_0_ring_emit_pipeline_sync - wait for the ring's last fence (the
 * IB that follows reads what the work before it wrote)
 */
static void sdma_v3_0_ring_emit_pipeline_sync(struct frb_ring *ring)
{
	uint32_t seq = ring->sync_seq;
	uint64_t addr = ring->fence_gpu_addr;

	/* wait for idle */
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
		       SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(0) |
		       SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3) | /* equal */
		       SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(1));
	frb_ring_write(ring, addr & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(addr) & 0xffffffff);
	frb_ring_write(ring, seq); /* reference */
	frb_ring_write(ring, 0xffffffff); /* mask */
	frb_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
		       SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(4)); /* retry count, poll interval */
}

/*
 * Another engine's fence (the GFX ring's, for the accelerator's 2D work):
 * the pipeline sync's packet with the compare function and memory given.
 */
static void sdma_v3_0_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
{
	frb_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
		       SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(0) |
		       SDMA_PKT_POLL_REGMEM_HEADER_FUNC(func) |
		       SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(1));
	frb_ring_write(ring, addr & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(addr) & 0xffffffff);
	frb_ring_write(ring, ref);
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
		       SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(4));
}

static const struct frb_ring_funcs sdma_v3_0_ring_funcs = {
	.align_mask = 0xf,
	.nop = SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP),
	.support_64bit_ptrs = 0,
	.get_rptr = sdma_v3_0_ring_get_rptr,
	.get_wptr = sdma_v3_0_ring_get_wptr,
	.set_wptr = sdma_v3_0_ring_set_wptr,
	.emit_ib = sdma_v3_0_ring_emit_ib,
	.emit_fence = sdma_v3_0_ring_emit_fence,
	.emit_hdp_flush = sdma_v3_0_ring_emit_hdp_flush,
	.insert_nop = sdma_v3_0_ring_insert_nop,
	.pad_ib = sdma_v3_0_ring_pad_ib,
	.emit_wreg = sdma_v3_0_ring_emit_wreg,
	.emit_pipeline_sync = sdma_v3_0_ring_emit_pipeline_sync,
	.emit_wait_mem = sdma_v3_0_ring_emit_wait_mem,
};

/*
 * sdma_v3_0_emit_copy_buffer - copy buffer using the sDMA engine
 * (byte_count bytes from src_offset to dst_offset, GPU addresses)
 */
static void sdma_v3_0_emit_copy_buffer(struct frb_device *adev, struct frb_ib *ib,
				       uint64_t src_offset,
				       uint64_t dst_offset,
				       uint32_t byte_count,
				       uint32_t copy_flags)
{
	(void)copy_flags;
	frb_ib_write(adev, ib, SDMA_PKT_HEADER_OP(SDMA_OP_COPY) |
		     SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR));
	frb_ib_write(adev, ib, byte_count);
	frb_ib_write(adev, ib, 0); /* src/dst endian swap */
	frb_ib_write(adev, ib, lower_32_bits(src_offset));
	frb_ib_write(adev, ib, upper_32_bits(src_offset));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, upper_32_bits(dst_offset));
}

/*
 * sdma_v3_0_emit_fill_buffer - fill buffer using the sDMA engine.
 * The header's FILLSIZE stays 0, as in amdgpu, and FILLSIZE 0 is a BYTE
 * fill: the engine repeats src_data's low byte over byte_count bytes
 * (measured on the RX 550, both engines: 0xa3b2c3d4 filled 0xd4d4d4d4).
 * amdgpu's callers pass 0 (clears); its 0xd0bed0be poison lands as 0xbe
 * bytes. Mesa asks for FILLSIZE 2 (dword) instead; not used here.
 */
static void sdma_v3_0_emit_fill_buffer(struct frb_device *adev, struct frb_ib *ib,
				       uint32_t src_data,
				       uint64_t dst_offset,
				       uint32_t byte_count)
{
	frb_ib_write(adev, ib, SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, upper_32_bits(dst_offset));
	frb_ib_write(adev, ib, src_data);
	frb_ib_write(adev, ib, byte_count);
}

/* sdma_v3_0_buffer_funcs */
#define SDMA_COPY_MAX_BYTES	0x3fffe0	/* not 0x3fffff due to HW limitation */
#define SDMA_COPY_NUM_DW	7
#define SDMA_FILL_MAX_BYTES	0x3fffe0	/* not 0x3fffff due to HW limitation */
#define SDMA_FILL_NUM_DW	5

static struct frb_ring *sdma_ring(struct frb_device *adev, int instance)
{
	struct frb_sdma *sdma = adev->sdma;

	if (!sdma || instance < 0 || instance >= sdma->num_instances)
		return NULL;
	return &sdma->instance[instance].ring;
}

/* the accelerator's 2D work runs on SDMA0's ring; NULL unless it is up */
struct frb_ring *frb_sdma_v3_0_ring(struct frb_device *adev, int instance)
{
	struct frb_ring *ring = sdma_ring(adev, instance);

	return ring && ring->ready ? ring : NULL;
}

/* sdma_v3_0_ring_pad_ib, for an IB built elsewhere (frb_rect.c) */
void frb_sdma_v3_0_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	sdma_v3_0_ring_pad_ib(ring, ib);
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

/*
 * amdgpu_copy_buffer (amdgpu_ttm.c) on one SDMA ring, polled: byte_count
 * bytes from src to dst, GPU addresses (VRAM now; GTT the same way once
 * the GART maps system pages), split at copy_max_bytes.
 */
int frb_sdma_v3_0_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset,
			      uint64_t dst_offset, uint32_t byte_count)
{
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t max_bytes = SDMA_COPY_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * SDMA_COPY_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size_in_bytes = byte_count < max_bytes ? byte_count : max_bytes;

		sdma_v3_0_emit_copy_buffer(adev, &ib, src_offset, dst_offset, cur_size_in_bytes, 0);
		src_offset += cur_size_in_bytes;
		dst_offset += cur_size_in_bytes;
		byte_count -= cur_size_in_bytes;
	}
	return sdma_submit_and_wait(ring, &ib, "copy");
}

/*
 * amdgpu_ttm_fill_mem on one SDMA ring, polled: byte_count bytes at dst_addr,
 * each src_data's low byte (sdma_v3_0_emit_fill_buffer)
 */
int frb_sdma_v3_0_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data,
			      uint64_t dst_addr, uint32_t byte_count)
{
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t max_bytes = SDMA_FILL_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * SDMA_FILL_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size = byte_count < max_bytes ? byte_count : max_bytes;

		sdma_v3_0_emit_fill_buffer(adev, &ib, src_data, dst_addr, cur_size);
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

static int sdma_test_buffers(struct frb_device *adev, struct frb_sdma *sdma)
{
	int r, i;

	for (i = 0; i < 2; i++) {
		if (sdma->test_bo[i])
			continue;
		r = frb_vram_obj_alloc(adev, FRB_SDMA_TEST_BYTES, 4096, &sdma->test_bo[i]);
		if (r)
			return r;
	}
	return 0;
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
	struct frb_sdma *sdma = adev->sdma;
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t total = FRB_SDMA_TEST_BYTES / 4, bytes = FRB_SDMA_TEST_BYTES - FRB_SDMA_TEST_GUARD;
	uint32_t src, dst, seed, value, i;
	uint64_t t0;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	r = sdma_test_buffers(adev, sdma);
	if (r)
		return r;
	src = sdma->test_bo[0];
	dst = sdma->test_bo[1];
	sdma->test_runs++;
	seed = 0x5d3a0000u ^ ((uint32_t)instance << 12) ^ (sdma->test_runs * 0x01000193u);
	/*
	 * Four different bytes, the low one new each run: the fill must repeat
	 * the low byte (FILLSIZE 0, sdma_v3_0_emit_fill_buffer), and any other
	 * byte or a dword pattern shows.
	 */
	value = 0xa1b2c300u | ((0xd4u + sdma->test_runs * 0x1du) & 0xff);

	for (i = 0; i < total; i++) {
		if (!fill)
			frb_vram_write32(adev, src + 4 * i, test_pattern(seed, i));
		frb_vram_write32(adev, dst + 4 * i, FRB_SDMA_POISON);
	}
	/* the CPU's VRAM writes reach the engine through the HDP: frb_ring_commit flushes it */
	t0 = adev->os->time_ms(adev->os->cookie);
	if (fill)
		r = frb_sdma_v3_0_fill_buffer(adev, instance, value, frb_vram_gpu_addr(adev, dst), bytes);
	else
		r = frb_sdma_v3_0_copy_buffer(adev, instance, frb_vram_gpu_addr(adev, src),
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

int frb_sdma_v3_0_init(struct frb_device *adev)
{
	struct frb_sdma *sdma = adev->sdma;
	struct frb_ring *ring;
	char name[8];
	int r, i;

	if (adev->stage_done < FRB_STAGE_SMU || !adev->aper)
		return -EPERM;
	if (!sdma) {
		sdma = (struct frb_sdma *)adev->os->alloc(adev->os->cookie, sizeof(*sdma));
		if (!sdma)
			return -ENOMEM;
		adev->sdma = sdma;
	}

	/* sdma_v3_0_early_init */
	sdma->num_instances = SDMA_MAX_INSTANCE;	/* all but Stoney */
	r = sdma_v3_0_init_microcode(adev, sdma);
	if (r)
		return r;

	/* sdma_v3_0_sw_init: the rings (no doorbells, no IRQ sources) */
	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		name[0] = 's'; name[1] = 'd'; name[2] = 'm'; name[3] = 'a';
		name[4] = (char)('0' + i); name[5] = 0;
		r = frb_ring_init(adev, ring, name, &sdma_v3_0_ring_funcs, 1024);
		if (r)
			return r;
		ring->me = i;	/* sdma_v3_0_set_ring_funcs */
		frb_info(adev, "%s: microcode version 0x%x, feature %u (%s NOPs)", ring->name,
			 sdma->instance[i].fw_version, sdma->instance[i].feature_version,
			 sdma->instance[i].burst_nop ? "burst" : "single");
	}

	/* sdma_v3_0_hw_init */
	sdma->touched = 1;
	sdma_v3_0_init_golden_registers(adev);

	r = sdma_v3_0_start(adev, sdma);
	if (r)
		return r;

	/* amdgpu_device_ip_late_init's amdgpu_ib_ring_tests, for these two rings */
	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		r = sdma_v3_0_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
		if (r) {
			frb_err(adev, "IB test failed on %s (%d).", ring->name, r);
			ring->ready = 0;
			return r;
		}
	}
	return 0;
}

int frb_sdma_v3_0_test(struct frb_device *adev, int instance, int test)
{
	struct frb_ring *ring = sdma_ring(adev, instance);

	if (!ring)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		return sdma_v3_0_ring_test_ring(ring);
	case FRB_TEST_IB:
		if (!ring->ready)
			return -ENODEV;
		return sdma_v3_0_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_COPY:
		return sdma_test_copy_fill(adev, instance, 0);
	case FRB_TEST_FILL:
		return sdma_test_copy_fill(adev, instance, 1);
	default:
		return -EINVAL;
	}
}

/* sdma_v3_0_hw_fini: context switch off, both engines halted, before the driver lets go of VRAM */
void frb_sdma_v3_0_fini(struct frb_device *adev)
{
	struct frb_sdma *sdma = adev->sdma;

	if (!sdma)
		return;
	if (sdma->touched && sdma->num_instances) {
		sdma_v3_0_ctx_switch_enable(adev, sdma, 0);
		sdma_v3_0_enable(adev, sdma, 0);
		frb_info(adev, "sdma: both engines halted (hw_fini)");
	}
	adev->os->free(adev->os->cookie, sdma);
	adev->sdma = NULL;
}
