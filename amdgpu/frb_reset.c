/*
 * GPU recovery: the parts of amdgpu_device_gpu_recover (amdgpu_device.c) and
 * amdgpu_job_timedout (amdgpu_job.c) a Polaris without a GPU scheduler
 * needs, run when a ring stops moving (the accelerator's watchdog) or by
 * hand (frbctl recover, the accelerator's CP_RECOVER):
 *
 *   1. amdgpu_ring_soft_recovery: kill the GFX ring's waves (SQ_CMD) for up
 *      to 10 ms, until its oldest unsignalled fence comes. A shader that
 *      never ends is cured here, with nothing reset.
 *   2. amdgpu_device_pre_asic_reset's IP soft reset: check_soft_reset on
 *      each block (GFX, then SDMA, amdgpu's IP order), then pre_soft_reset,
 *      soft_reset and post_soft_reset on the hung ones; their rings' fences
 *      forced complete (amdgpu_fence_driver_force_completion).
 *   3. check_soft_reset again: a block still busy, or a ring that failed its
 *      test in post_soft_reset, means a full reset is needed.
 *
 * WHERE FIREBRAND DIFFERS, AND WHY.
 * - vi_need_full_reset() is true on Polaris ("change this when we support
 *   soft reset"), so amdgpu skips step 2 and resets the whole ASIC (BACO or
 *   the PCI config reset), then brings every block back, the display from
 *   its atomic state and VRAM from shadow copies. Firebrand runs step 2:
 *   it keeps the display, VRAM and the GART as they are, and it is what
 *   RadeonNI's rdnctl recover did on CIK (the fold scope's parity list,
 *   item 7). A full reset at run time is not ported: when step 3 fails,
 *   recovery says so and the driver must be reloaded, and a load resets the
 *   ASIC (stage post, vi_asic_reset).
 * - The fences are forced complete after the blocks are reset, not before:
 *   with polled fences, an engine still running could otherwise write an
 *   older sequence over the forced one.
 * - The work on a reset ring is dropped, not resubmitted (no jobs, no
 *   guilty context): its fences signal and the clients go on.
 *
 * SOC15 (Vega 20: gfx_v9_0, sdma_v4_0). amdgpu there has the soft recovery,
 * then the KIQ's queue reset (gfx_v9_0_reset_kgq; Firebrand has no KIQ),
 * then a reset of the whole ASIC: no block has check_soft_reset, and
 * sdma_v4_0_soft_reset is "todo". Firebrand runs the same three steps as on
 * Polaris over frb_gfx_v9_0_reset_funcs (amdgpu's gfx_v9_0_soft_reset, split)
 * and frb_sdma_v4_0_reset_funcs (SDMAn_VIRT_RESET_REQ: frb_sdma_v4_0.c's
 * header). One more difference there: when SDMA and GFX both read busy and
 * GFX was not forced, SDMA is reset first and its fences forced, and GFX is
 * reset only if its fence still stands 200 ms later. The accelerator's GFX
 * ring waits on SDMA0's fence (the join, a WAIT_REG_MEM that reads as a busy
 * CP), so a stalled SDMA takes GFX with it, and the forced SDMA fence lets
 * GFX go on with its state, its clients' contexts and the clear state kept.
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_ring.h"
#include "include/asic_reg/oss/oss_3_0_d.h"
#include "include/asic_reg/gca/gfx_8_0_d.h"

#define SDMA1_REGISTER_OFFSET	0x200	/* not a register */

/* amdgpu_ring_soft_recovery's deadline */
#define SOFT_RECOVERY_US	10000

#define NUM_BLOCKS	2
static const struct frb_ip_reset_funcs *const vi_blocks[NUM_BLOCKS] = {
	&frb_gfx_v8_0_reset_funcs,	/* FRB_RECOVER_GFX */
	&frb_sdma_v3_0_reset_funcs,	/* FRB_RECOVER_SDMA */
};
static const struct frb_ip_reset_funcs *const soc15_blocks[NUM_BLOCKS] = {
	&frb_gfx_v9_0_reset_funcs,
	&frb_sdma_v4_0_reset_funcs,
};

/* GFX given time to go on by itself once SDMA's fences are forced (the file header) */
#define GFX_AFTER_SDMA_US	200000

/*
 * amdgpu_fence_driver_force_completion on the rings a block's reset threw
 * away. They are taken before the reset: frb_*_ring returns NULL for a ring
 * that is not ready, which the reset leaves until post_soft_reset's test.
 */
static void force_completion(struct frb_ring **rings, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (rings[i])
			frb_fence_driver_force_completion(rings[i]);
}

static void snapshot(struct frb_device *adev, struct frb_recover_report *rep)
{
	if (frb_is_soc15(adev)) {
		frb_soc15_recover_snapshot(adev, rep);	/* no SRBM on SOC15: those stay 0 */
		return;
	}
	rep->grbm_status = RREG32(mmGRBM_STATUS);
	rep->grbm_status2 = RREG32(mmGRBM_STATUS2);
	rep->srbm_status = RREG32(mmSRBM_STATUS);
	rep->srbm_status2 = RREG32(mmSRBM_STATUS2);
	rep->cp_stat = RREG32(mmCP_STAT);
	rep->sdma_status[0] = RREG32(mmSDMA0_STATUS_REG);
	rep->sdma_status[1] = RREG32(mmSDMA0_STATUS_REG + SDMA1_REGISTER_OFFSET);
}

/* amdgpu_ring_soft_recovery on the GFX ring, VMID 0 (every client's, until per-client VMs) */
static int soft_recovery(struct frb_device *adev)
{
	struct frb_ring *ring = frb_gfx_ring(adev);
	uint32_t seq, t;
	uint64_t t0;

	if (!ring)
		return 0;
	seq = frb_fence_read(ring);
	if ((int32_t)(ring->sync_seq - seq) <= 0)
		return 0;	/* nothing outstanding on it */
	seq++;			/* the oldest unsignalled fence */
	t0 = adev->os->time_ms(adev->os->cookie);
	for (t = 0; t < SOFT_RECOVERY_US; t += 10) {
		if ((int32_t)(frb_fence_read(ring) - seq) >= 0)
			return 1;
		/* the deadline by the clock too: on SOC15 each round is an RLC safe-mode handshake */
		if (adev->os->time_ms(adev->os->cookie) - t0 > 2 * SOFT_RECOVERY_US / 1000)
			break;
		if (frb_is_soc15(adev))
			frb_gfx_v9_0_ring_soft_recovery(ring, 0);
		else
			frb_gfx_v8_0_ring_soft_recovery(ring, 0);
		udelay(10);
	}
	return (int32_t)(frb_fence_read(ring) - seq) >= 0;
}

/* pre, soft and post on one block, its rings' fences forced in between; the ring tests' result */
static int reset_block(struct frb_device *adev, const struct frb_ip_reset_funcs *b, struct frb_ring **rings)
{
	b->pre_soft_reset(adev);
	b->soft_reset(adev);
	/* the block is halted and reset: nothing writes its fences now */
	force_completion(rings, 2);
	return b->post_soft_reset(adev);
}

/* nonzero when the ring's fence moves, or has nothing left behind it, within GFX_AFTER_SDMA_US */
static int ring_goes_on(struct frb_ring *ring)
{
	struct frb_device *adev;
	uint32_t first, now, t;

	if (!ring)
		return 0;
	adev = ring->adev;
	first = frb_fence_read(ring);
	for (t = 0; t < GFX_AFTER_SDMA_US; t += 100) {
		now = frb_fence_read(ring);
		if (now == ring->sync_seq || now != first)
			return 1;
		udelay(100);
	}
	return 0;
}

int frb_device_gpu_recover(struct frb_device *adev, uint32_t force, struct frb_recover_report *rep)
{
	const struct frb_ip_reset_funcs *const *reset_blocks = frb_is_soc15(adev) ? soc15_blocks : vi_blocks;
	struct frb_recover_report local;
	struct frb_ring *rings[2][2];
	uint64_t t0 = adev->os->time_ms(adev->os->cookie);
	uint32_t hung = 0, bit;
	unsigned i;
	int r;

	if (!rep)
		rep = &local;
	memset(rep, 0, sizeof(*rep));
	if (adev->stage_done < FRB_STAGE_GFX)
		return -ENODEV;
	/* SI, CIK: gfx_v6_0/si_dma's and gfx_v7_0/cik_sdma's soft reset are not ported (VI registers below) */
	if (frb_is_si(adev) || frb_is_cik(adev))
		return -ENOSYS;
	snapshot(adev, rep);
	frb_info(adev, "recover: GRBM_STATUS %08x GRBM_STATUS2 %08x SRBM_STATUS %08x SRBM_STATUS2 %08x "
		 "CP_STAT %08x SDMA0/1_STATUS %08x %08x%s",
		 rep->grbm_status, rep->grbm_status2, rep->srbm_status, rep->srbm_status2, rep->cp_stat,
		 rep->sdma_status[0], rep->sdma_status[1], force ? " (forced)" : "");

	/* 1. amdgpu_job_timedout: the soft recovery first */
	if (!force && soft_recovery(adev)) {
		rep->soft_recovered = 1;
		adev->gpu_reset_counter++;
		rep->ms = (uint32_t)(adev->os->time_ms(adev->os->cookie) - t0);
		frb_info(adev, "recover: ring gfx soft recovered (the waves were killed; nothing reset)");
		return 0;
	}

	/* 2. amdgpu_device_ip_check_soft_reset, then pre / soft / post on the hung blocks */
	for (i = 0; i < NUM_BLOCKS; i++) {
		bit = 1u << i;
		if (force & bit) {
			reset_blocks[i]->force_soft_reset(adev);
			hung |= bit;
		} else if (reset_blocks[i]->check_soft_reset(adev)) {
			hung |= bit;
		}
	}
	rep->hung = hung;
	if (!hung) {
		rep->ms = (uint32_t)(adev->os->time_ms(adev->os->cookie) - t0);
		frb_info(adev, "recover: no block is busy; nothing to reset");
		return 0;
	}
	/* the rings, while the blocks still call them ready */
	rings[0][0] = frb_gfx_ring(adev);
	rings[0][1] = NULL;
	rings[1][0] = frb_sdma_ring(adev, 0);
	rings[1][1] = frb_sdma_ring(adev, 1);

	if (frb_is_soc15(adev) && hung == (FRB_RECOVER_GFX | FRB_RECOVER_SDMA) && !(force & FRB_RECOVER_GFX)) {
		/* SDMA first; GFX only if it does not go on once SDMA's fences are forced (the file header) */
		r = reset_block(adev, reset_blocks[1], rings[1]);
		if (r) {
			rep->ring_failed |= FRB_RECOVER_SDMA;
			frb_err(adev, "recover: %s: a ring failed its test after the soft reset (%d)",
				reset_blocks[1]->name, r);
		}
		if (ring_goes_on(rings[0][0])) {
			hung &= ~FRB_RECOVER_GFX;
			rep->hung = hung;
			frb_info(adev, "recover: gfx goes on with sdma's fences forced: it was waiting on sdma, not hung");
		} else {
			r = reset_block(adev, reset_blocks[0], rings[0]);
			if (r) {
				rep->ring_failed |= FRB_RECOVER_GFX;
				frb_err(adev, "recover: %s: a ring failed its test after the soft reset (%d)",
					reset_blocks[0]->name, r);
			}
		}
	} else {
		for (i = 0; i < NUM_BLOCKS; i++)
			if (hung & (1u << i))
				reset_blocks[i]->pre_soft_reset(adev);
		for (i = 0; i < NUM_BLOCKS; i++)
			if (hung & (1u << i))
				reset_blocks[i]->soft_reset(adev);
		/* the blocks are halted and reset: nothing writes their fences now */
		for (i = 0; i < NUM_BLOCKS; i++)
			if (hung & (1u << i))
				force_completion(rings[i], 2);
		for (i = 0; i < NUM_BLOCKS; i++) {
			if (!(hung & (1u << i)))
				continue;
			r = reset_blocks[i]->post_soft_reset(adev);
			if (r) {
				rep->ring_failed |= 1u << i;
				frb_err(adev, "recover: %s: a ring failed its test after the soft reset (%d)",
					reset_blocks[i]->name, r);
			}
		}
	}

	/* 3. amdgpu_device_ip_check_soft_reset again (only the blocks that were reset: one left alone is at work) */
	for (i = 0; i < NUM_BLOCKS; i++)
		if ((hung & (1u << i)) && reset_blocks[i]->check_soft_reset(adev))
			rep->still_hung |= 1u << i;
	rep->ms = (uint32_t)(adev->os->time_ms(adev->os->cookie) - t0);
	if (rep->still_hung || rep->ring_failed) {
		snapshot(adev, rep);
		frb_err(adev, "recover: soft reset of %s%s did not recover (still busy %x, rings failed %x; "
			"GRBM_STATUS %08x GRBM_STATUS2 %08x SRBM_STATUS2 %08x SDMA0/1_STATUS %08x %08x): a full reset "
			"is needed, which a reload of the driver does",
			(hung & FRB_RECOVER_GFX) ? "gfx " : "", (hung & FRB_RECOVER_SDMA) ? "sdma" : "",
			rep->still_hung, rep->ring_failed, rep->grbm_status, rep->grbm_status2, rep->srbm_status2,
			rep->sdma_status[0], rep->sdma_status[1]);
		return -EIO;
	}
	adev->gpu_reset_counter++;
	frb_info(adev, "recover: soft reset of %s%s: rings back after %u ms (reset %u)",
		 (hung & FRB_RECOVER_GFX) ? "gfx " : "", (hung & FRB_RECOVER_SDMA) ? "sdma" : "",
		 rep->ms, adev->gpu_reset_counter);
	return 0;
}
