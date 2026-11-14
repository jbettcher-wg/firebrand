/*
 * amdgpu_uvd.c (Linux 7.0.2), the parts the decode ring of Polaris 12
 * needs: sw_init (the firmware file, its version, the handle count, the
 * VCPU bo), resume (the firmware copied into the VCPU bo), the kernel's
 * create/destroy messages and their IB (send_msg), the IB test amdgpu runs
 * from late_init, begin_use and the idle work without powerplay.
 *
 * DEVIATIONS from amdgpu, each with its consequence:
 * - PLACEMENT. The VCPU bo and the msg bo (amdgpu: ib_bo, GTT) live in a
 *   VRAM region of their own at the top of the CPU window (frb_gmc_v8_0.c
 *   frb_gmc_layout, layout.uvd); amdgpu allocates the VCPU bo top-down in
 *   VRAM (0xF4_7F80_0000 on the E9173). UVD 6 addresses 64 bits
 *   (address_64_bit), so neither needs the 256 MB segment older UVD did.
 *   The IB test's IB comes out of frb_ring.c's VRAM IB pool (amdgpu: the
 *   direct pool, GTT).
 * - FENCES are the ring's writeback slot (frb_ring_init), polled; amdgpu
 *   puts UVD's fence word behind the firmware in the VCPU bo
 *   (amdgpu_fence_driver_start_ring). Either is an address the VCPU
 *   writes through its LMI; nothing reads it but the driver.
 * - NO DELAYED WORK. amdgpu_uvd_ring_begin_use cancels the idle work and
 *   brings the clocks and the VCPU up if it had run; end_use schedules it
 *   a second later. Here the idle work (frb_uvd_idle_work_handler) runs
 *   when the caller knows the ring is idle: at the end of stage uvd (the
 *   state Linux reaches a second after its IB tests) and when the last
 *   video session ends.
 * - CLOCKS. Without powerplay amdgpu takes the fixed 533/400 MHz
 *   (vi_set_uvd_clocks) at begin_use and 0 in the idle work; with it
 *   (Linux's capture) powerplay's UVD DPM levels do instead. Firebrand does
 *   the same: with FirebrandDPM, smu7_powergate_uvd's messages
 *   (frb_smu7_clockpowergating.c, replay-uvd-dpm); without it, the fixed
 *   arm, whose CG_VCLK/DCLK_CNTL writes are where the capture has SMC
 *   messages (replay-uvd, docs/stage-verification.md, uvd).
 * - MEMORY. The VCPU bo is written through BAR0 with little-endian words
 *   (memcpy_toio of the file's bytes); the messages are little-endian
 *   words (cpu_to_le32), whatever the ring's own byte order.
 *
 * Copyright 2011 Advanced Micro Devices, Inc.
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
 * Authors: Christian König <deathsimple@vodafone.de>
 */
#include "frb_uvd.h"
#include "frb_pm.h"

/* Polaris10/11 firmware version */
#define FW_1_66_16	((1 << 24) | (66 << 16) | (16 << 8))

#define FIRMWARE_POLARIS12	"polaris12_uvd.bin"

/*
 * amdgpu_uvd_sw_init: the firmware, its version and handle count, the
 * VCPU bo (VRAM, the region frb_gmc_layout keeps) and the msg bo.
 */
int frb_uvd_sw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t bo_size, family_id, version_major, version_minor;
	const char *fw_name;
	int r;

	switch (adev->asic_type) {
	case CHIP_POLARIS12:
		fw_name = FIRMWARE_POLARIS12;
		break;
	default:
		frb_err(adev, "amdgpu_uvd: only Polaris 12 is wired up");
		return -EINVAL;
	}

	uvd->fw.data = 0;
	uvd->fw.size = 0;
	r = adev->os->firmware(adev->os->cookie, fw_name, &uvd->fw.data, &uvd->fw.size);
	if (!r)
		r = frb_ucode_parse(adev, fw_name, &uvd->fw, &uvd->ucode);
	if (r) {
		frb_err(adev, "amdgpu_uvd: Can't validate firmware \"%s\"", fw_name);
		return r;
	}

	/* Set the default UVD handles that the firmware can handle */
	uvd->max_handles = AMDGPU_DEFAULT_UVD_HANDLES;

	family_id = uvd->ucode.ucode_version & 0xff;
	version_major = (uvd->ucode.ucode_version >> 24) & 0xff;
	version_minor = (uvd->ucode.ucode_version >> 8) & 0xff;
	frb_info(adev, "Found UVD firmware Version: %u.%u Family ID: %u",
		 version_major, version_minor, family_id);

	/*
	 * Limit the number of UVD handles depending on microcode major
	 * and minor versions. The firmware version which has 40 UVD
	 * instances support is 1.80. So all subsequent versions should
	 * also have the same support.
	 */
	if ((version_major > 0x01) ||
	    ((version_major == 0x01) && (version_minor >= 0x50)))
		uvd->max_handles = AMDGPU_MAX_UVD_HANDLES;

	uvd->fw_version = ((version_major << 24) | (version_minor << 16) |
			   (family_id << 8));

	bo_size = AMDGPU_UVD_STACK_SIZE + AMDGPU_UVD_HEAP_SIZE
		  + AMDGPU_UVD_SESSION_SIZE * uvd->max_handles;
	/* no PSP on VI: the driver loads the firmware */
	bo_size += AMDGPU_GPU_PAGE_ALIGN(uvd->ucode.ucode_size_bytes + 8);

	/* amdgpu_bo_create_kernel(VRAM | GTT): the layout's UVD region */
	if (!adev->layout.uvd_size ||
	    bo_size + FRB_UVD_IB_BO_SIZE > adev->layout.uvd_size ||
	    !frb_vram_ok(adev, adev->layout.uvd, bo_size + FRB_UVD_IB_BO_SIZE)) {
		frb_err(adev, "(%d) failed to allocate UVD bo (%u bytes, region %u at 0x%08x)", -ENOMEM,
			bo_size, adev->layout.uvd_size, adev->layout.uvd);
		return -ENOMEM;
	}
	uvd->vcpu_bo = adev->layout.uvd;
	uvd->vcpu_bo_size = bo_size;
	uvd->gpu_addr = frb_vram_gpu_addr(adev, uvd->vcpu_bo);

	/* from uvd v5.0 HW addressing capacity increased to 64 bits */
	uvd->address_64_bit = 1;

	/* amdgpu_uvd_create_msg_bo_helper(128 KB): address_64_bit, so no segment rule */
	uvd->ib_bo = uvd->vcpu_bo + AMDGPU_GPU_PAGE_ALIGN(bo_size);
	uvd->ib_bo_gpu_addr = frb_vram_gpu_addr(adev, uvd->ib_bo);

	/* default: CHIP_POLARIS10 and later */
	uvd->use_ctx_buf = adev->asic_type >= CHIP_POLARIS10;

	if ((adev->asic_type == CHIP_POLARIS10 || adev->asic_type == CHIP_POLARIS11) &&
	    uvd->fw_version < FW_1_66_16)
		frb_err(adev, "POLARIS10/11 UVD firmware version %u.%u is too old.",
			version_major, version_minor);

	frb_info(adev, "uvd: VCPU bo %u bytes at %llx (BAR0 0x%08x), msg bo at %llx; %u handles",
		 bo_size, (unsigned long long)uvd->gpu_addr, uvd->vcpu_bo,
		 (unsigned long long)uvd->ib_bo_gpu_addr, uvd->max_handles);
	return 0;
}

/*
 * amdgpu_uvd_resume, the arm without a saved bo: the ucode bytes at the
 * start of the VCPU bo (memcpy_toio: the file's byte order, here as
 * little-endian words), the rest of the bo zeroed (memset_io), then the
 * fence forced to its last sequence (amdgpu_fence_driver_force_completion).
 */
int frb_uvd_resume(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t size = uvd->vcpu_bo_size, ptr = uvd->vcpu_bo, offset, i;
	uint64_t t0 = adev->os->time_ms(adev->os->cookie);

	if (!uvd->vcpu_bo)
		return -EINVAL;

	offset = uvd->ucode.ucode_array_offset;
	for (i = 0; i < uvd->ucode.ucode_size_bytes; i += 4)
		frb_vram_write32(adev, ptr + i, frb_get_le32(uvd->fw.data, offset + i));
	size -= uvd->ucode.ucode_size_bytes;
	ptr += uvd->ucode.ucode_size_bytes;
	for (i = 0; i < size; i += 4)
		frb_vram_write32(adev, ptr + i, 0);
	/* the VCPU fetches it through the MC: the CPU's BAR0 writes out of the HDP first */
	frb_flush_hdp(adev);

	/* to restore uvd fence seq */
	if (uvd->ring.funcs)
		frb_fence_driver_force_completion(&uvd->ring);
	frb_info(adev, "uvd: firmware %u bytes into the VCPU bo, %u zeroed after it (%llu ms)",
		 uvd->ucode.ucode_size_bytes, size,
		 (unsigned long long)(adev->os->time_ms(adev->os->cookie) - t0));
	return 0;
}

/*
 * amdgpu_uvd_send_msg, direct: an IB of 16 dwords that hands the VCPU the
 * msg bo (GPCOM_VCPU_DATA0/1 = its address, CMD 0 = MSG_BUFFER), NO_OPs
 * after it, submitted and given a fence; *seq is that fence.
 */
static int amdgpu_uvd_send_msg(struct frb_ring *ring, uint64_t addr, struct frb_ib *ibp, uint32_t *seq)
{
	struct frb_device *adev = ring->adev;
	uint32_t offset, data[4];
	struct frb_ib ib;
	int i, r;

	r = frb_ib_get(adev, 64, &ib);
	if (r)
		return r;

	offset = UVD_BASE_SI;

	data[0] = FRB_UVD_PACKET0(offset + UVD_GPCOM_VCPU_DATA0);
	data[1] = FRB_UVD_PACKET0(offset + UVD_GPCOM_VCPU_DATA1);
	data[2] = FRB_UVD_PACKET0(offset + UVD_GPCOM_VCPU_CMD);
	data[3] = FRB_UVD_PACKET0(offset + UVD_NO_OP);

	frb_uvd_ib_write(adev, &ib, data[0]);
	frb_uvd_ib_write(adev, &ib, (uint32_t)addr);
	frb_uvd_ib_write(adev, &ib, data[1]);
	frb_uvd_ib_write(adev, &ib, (uint32_t)(addr >> 32));
	frb_uvd_ib_write(adev, &ib, data[2]);
	frb_uvd_ib_write(adev, &ib, 0);
	for (i = 6; i < 16; i += 2) {
		frb_uvd_ib_write(adev, &ib, data[3]);
		frb_uvd_ib_write(adev, &ib, 0);
	}

	/* amdgpu_job_submit_direct -> amdgpu_ib_schedule, whose ring_alloc calls begin_use */
	frb_uvd_ring_begin_use(ring);
	r = frb_ib_schedule(ring, &ib, seq);
	if (r) {
		frb_ib_free(adev, &ib);
		return r;
	}
	*ibp = ib;	/* frb_ib_free it once *seq has signalled */
	return 0;
}

/* the kernel's msg: little-endian words at the msg bo, the rest of its first 4 KB zero */
static void uvd_msg_write(struct frb_device *adev, const uint32_t *head, uint32_t n)
{
	uint32_t i, bo = adev->uvd->ib_bo;

	for (i = 0; i < 1024; ++i)
		frb_vram_write32(adev, bo + 4 * i, i < n ? head[i] : 0);
}

/*
 * multiple fence commands without any stream commands in between can
 * crash the vcpu so just try to emmit a dummy create/destroy msg to
 * avoid this
 */
int frb_uvd_get_create_msg(struct frb_ring *ring, uint32_t handle, struct frb_ib *ib, uint32_t *seq)
{
	struct frb_device *adev = ring->adev;
	uint32_t msg[11];

	/* stitch together an UVD create msg */
	msg[0] = 0x00000de4;
	msg[1] = 0x00000000;
	msg[2] = handle;
	msg[3] = 0x00000000;
	msg[4] = 0x00000000;
	msg[5] = 0x00000000;
	msg[6] = 0x00000000;
	msg[7] = 0x00000780;
	msg[8] = 0x00000440;
	msg[9] = 0x00000000;
	msg[10] = 0x01b37000;
	uvd_msg_write(adev, msg, 11);

	return amdgpu_uvd_send_msg(ring, adev->uvd->ib_bo_gpu_addr, ib, seq);
}

int frb_uvd_get_destroy_msg(struct frb_ring *ring, uint32_t handle, struct frb_ib *ib, uint32_t *seq)
{
	struct frb_device *adev = ring->adev;
	uint32_t msg[4];

	/* stitch together an UVD destroy msg */
	msg[0] = 0x00000de4;
	msg[1] = 0x00000002;
	msg[2] = handle;
	msg[3] = 0x00000000;
	uvd_msg_write(adev, msg, 4);

	return amdgpu_uvd_send_msg(ring, adev->uvd->ib_bo_gpu_addr, ib, seq);
}

/*
 * amdgpu_uvd_idle_work_handler: when no fence is outstanding, with DPM
 * amdgpu_dpm_enable_uvd(false) (smu7_powergate_uvd: power- and clock-gated,
 * UVDDPM_Disable), without it the clocks to 0, the block power-gated
 * (uvd_v6_0_stop) and clock-gated. Returns -EBUSY (and does nothing) while
 * work is queued.
 */
int frb_uvd_idle_work_handler(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t fences;

	if (!uvd || !uvd->ring.funcs)
		return -ENODEV;
	fences = uvd->ring.sync_seq - frb_fence_read(&uvd->ring);	/* amdgpu_fence_count_emitted */
	if (fences)
		return -EBUSY;
	if (!uvd->powered)
		return 0;

	if (frb_pm_enabled(adev)) {
		frb_pm_enable_uvd(adev, 0);
	} else {
		frb_vi_set_uvd_clocks(adev, 0, 0);
		/* shutdown the UVD block */
		frb_uvd_v6_0_set_powergating_state(adev, 1);
		frb_uvd_v6_0_set_clockgating_state(adev, 1);
	}
	uvd->powered = 0;
	return 0;
}

/*
 * amdgpu_uvd_ring_begin_use: the idle work had run (or nothing has started
 * the block). With DPM amdgpu_dpm_enable_uvd(true) (smu7_powergate_uvd:
 * ungated, the VCPU started, the UVD table's boot level, UVDDPM_Enable);
 * without it 533/400 MHz, ungated, the VCPU started.
 */
void frb_uvd_ring_begin_use(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	struct frb_uvd *uvd = adev->uvd;
	int r;

	if (uvd->powered)
		return;
	/* the flag first: start()'s failure is the ring test's to report, as on Linux */
	uvd->powered = 1;
	if (frb_pm_enabled(adev)) {
		/* smu7_powergate_uvd(false): ungated, the VCPU started, UVD's DPM levels on */
		frb_pm_enable_uvd(adev, 1);
		return;
	}
	r = frb_vi_set_uvd_clocks(adev, 53300, 40000);
	if (r)
		frb_err(adev, "uvd: vi_set_uvd_clocks(533, 400 MHz) failed (%d)", r);
	frb_uvd_v6_0_set_clockgating_state(adev, 0);
	frb_uvd_v6_0_set_powergating_state(adev, 0);
}

/*
 * amdgpu_uvd_ring_test_ib: a create msg for handle 1 and its fence, then
 * the destroy msg and its fence.
 */
int frb_uvd_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
{
	struct frb_device *adev = ring->adev;
	uint32_t seq = 0, handle = 1;
	uint64_t t0 = adev->os->time_ms(adev->os->cookie);
	struct frb_ib ib;
	int r;

	r = frb_uvd_get_create_msg(ring, handle, &ib, &seq);
	if (r)
		goto error;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		frb_err(adev, "%s: IB test: the create msg's fence %u did not come (fence %u)", ring->name, seq,
			frb_fence_read(ring));
		goto error;
	}
	/* the IB pool's newest IB goes back once its fence has signalled (never after a timeout) */
	frb_ib_free(adev, &ib);

	r = frb_uvd_get_destroy_msg(ring, handle, &ib, &seq);
	if (r)
		goto error;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		frb_err(adev, "%s: IB test: the destroy msg's fence %u did not come (fence %u)", ring->name, seq,
			frb_fence_read(ring));
		goto error;
	}
	frb_ib_free(adev, &ib);
	adev->uvd->ib_tests++;
	frb_info(adev, "%s: IB test passed: create and destroy msg for handle %u, fence %u (%llu ms)",
		 ring->name, handle, seq, (unsigned long long)(adev->os->time_ms(adev->os->cookie) - t0));
error:
	return r;
}
