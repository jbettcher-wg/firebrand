/*
 * amdgpu_uvd.c (Linux 7.0.2; the Vega 20 arms from 7.1.1, the kernel of
 * that card's capture), the parts the decode rings of Polaris 12 (UVD 6.3)
 * and Vega 20 (UVD 7.2, two instances) need: sw_init (the firmware file,
 * its version, the handle count, each instance's VCPU bo), resume (the
 * firmware copied into the VCPU bo; on Vega 20 the PSP has loaded it into
 * its TMR and the bo is only zeroed), the kernel's create/destroy messages
 * and their IB (send_msg), the IB test amdgpu runs from late_init,
 * begin_use and the idle work with and without powerplay.
 *
 * DEVIATIONS from amdgpu, each with its consequence:
 * - PLACEMENT. The VCPU bos and the msg bo (amdgpu: ib_bo, GTT) live in a
 *   VRAM region of their own at the top of the CPU window (frb_gmc_v8_0.c
 *   frb_gmc_layout / frb_gmc_v9_0.c frb_gmc_v9_0_layout, layout.uvd);
 *   amdgpu allocates the VCPU bo top-down in VRAM (0xF4_7F80_0000 on the
 *   E9173; 0x83_FE80_0000 and 0x83_FE40_0000 on the MI50). UVD 6 and 7
 *   address 64 bits (address_64_bit), so neither needs the 256 MB segment
 *   older UVD did; SI's 3.1 and CIK's 4.2 do, and the CPU window is their
 *   VCPU bo's segment (frb_gmc_v6_0.c's and frb_gmc_v7_0.c's layouts), where
 *   the sessions put everything a decode names (FirebrandVideo.cpp). The IB test's IB comes out of frb_ring.c's VRAM IB pool
 *   (amdgpu: the direct pool, GTT).
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
 *   Vega 20: with FirebrandDPM, vega20_power_gate_uvd (the SMU's DPM_UVD
 *   feature on at begin_use, off in the idle work: frb_vega20_hwmgr.c);
 *   without it nothing at all, as amdgpu's arm there is three no-ops
 *   (soc15_set_uvd_clocks is commented out, uvd_v7_0 has no power gating
 *   and its clock gating state function is empty). SI: with DPM,
 *   amdgpu_dpm_enable_uvd's legacy arm, the VBIOS's UVD power state (its
 *   VCLK/DCLK on the UPLL, si_set_uvd_clocks, the engine and memory clocks
 *   held) at begin_use and the performance state in the idle work
 *   (frb_si_dpm_enable_uvd); without it si_set_uvd_clocks(533, 400 MHz) and
 *   bypass. Either way the VCPUs stay up between uses: only Polaris stops
 *   its VCPU in the idle work.
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
#include "frb_psp.h"
#include "frb_smu11.h"
#include "frb_soc15.h"

/* Polaris10/11 firmware version */
#define FW_1_66_16	((1 << 24) | (66 << 16) | (16 << 8))

#define FIRMWARE_POLARIS12	"polaris12_uvd.bin"
#define FIRMWARE_VEGA20		"vega20_uvd.bin"

static inline uint32_t swab32(uint32_t v)
{
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

/* --- what both UVD versions share with the sessions -------------------------- */

struct frb_ring *frb_uvd_ring(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t i;

	if (!uvd)
		return NULL;
	for (i = 0; i < uvd->num_uvd_inst; i++) {
		if (uvd->harvest_config & (1 << i))
			continue;
		return uvd->inst[i].ring.funcs && uvd->inst[i].ring.ready ? &uvd->inst[i].ring : NULL;
	}
	return NULL;
}

uint32_t frb_uvd_reg_base(struct frb_device *adev, int me)
{
	if (adev->asic_type >= CHIP_VEGA10)
		return adev->reg_offset[UVD_HWIP][me][1];
	return UVD_BASE_SI;
}

/*
 * amdgpu_ring_write for a UVD ring. Default: little-endian dwords, LMI swap
 * 0. Swapped (amdgpu's __BIG_ENDIAN arm): the CPU's order on a big-endian
 * host, which LMI swap 0xa turns round on the fetch; the dword is stored
 * byte-swapped relative to little-endian so the result is the same on any
 * host the library is built for.
 */
void frb_uvd_ring_write(struct frb_ring *ring, uint32_t v)
{
	frb_ring_write(ring, ring->adev->uvd->swapped ? swab32(v) : v);
}

void frb_uvd_ib_write(struct frb_device *adev, struct frb_ib *ib, uint32_t v)
{
	frb_ib_write(adev, ib, adev->uvd->swapped ? swab32(v) : v);
}

void frb_uvd_ib_pad(struct frb_device *adev, struct frb_ib *ib)
{
	uint32_t nop = FRB_UVD_PACKET0(frb_uvd_reg_base(adev, frb_uvd_ring(adev) ? frb_uvd_ring(adev)->me : 0) +
				       UVD_NO_OP);

	if (ib->length_dw & 1)
		frb_uvd_ib_write(adev, ib, 0);
	while (ib->length_dw & 0xf) {
		frb_uvd_ib_write(adev, ib, nop);
		frb_uvd_ib_write(adev, ib, 0);
	}
}
#define FIRMWARE_BONAIRE	"bonaire_uvd.bin"	/* amdgpu_uvd.c; radeon loads bonaire_uvd.bin for Hawaii too */
#define FIRMWARE_HAWAII		"hawaii_uvd.bin"
#define FIRMWARE_TAHITI		"tahiti_uvd.bin"
#define FIRMWARE_VERDE		"verde_uvd.bin"
#define FIRMWARE_PITCAIRN	"pitcairn_uvd.bin"

/*
 * amdgpu_uvd_sw_init: the firmware, its version and handle count, each
 * instance's VCPU bo (VRAM, the region the GMC's layout keeps) and the msg
 * bo. The caller's early_init has set num_uvd_inst and harvest_config.
 */
int frb_uvd_sw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t bo_size, family_id, version_major, version_minor, j, next, need;
	const char *fw_name;
	int r;

	switch (adev->asic_type) {
	case CHIP_TAHITI:
		fw_name = FIRMWARE_TAHITI;
		break;
	case CHIP_VERDE:
		fw_name = FIRMWARE_VERDE;
		break;
	case CHIP_PITCAIRN:
		fw_name = FIRMWARE_PITCAIRN;
		break;
	case CHIP_BONAIRE:
		fw_name = FIRMWARE_BONAIRE;
		break;
	case CHIP_HAWAII:
		fw_name = FIRMWARE_HAWAII;
		break;
	case CHIP_POLARIS12:
		fw_name = FIRMWARE_POLARIS12;
		break;
	case CHIP_VEGA20:
		fw_name = FIRMWARE_VEGA20;
		break;
	default:
		frb_err(adev, "amdgpu_uvd: only Polaris 12, Vega 20, SI and CIK are wired up");
		return -EINVAL;
	}
	if (!uvd->num_uvd_inst)
		uvd->num_uvd_inst = 1;
	/* adev->firmware.load_type: the PSP loads on SOC15 (stage smu did), the driver on VI */
	uvd->psp_load = frb_is_soc15(adev);

	uvd->fw_name = fw_name;
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
	version_major = version_minor = 0;

	if (adev->asic_type < CHIP_VEGA20) {
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
	} else {
		unsigned int enc_major, enc_minor, dec_minor;

		dec_minor = (uvd->ucode.ucode_version >> 8) & 0xff;
		enc_minor = (uvd->ucode.ucode_version >> 24) & 0x3f;
		enc_major = (uvd->ucode.ucode_version >> 30) & 0x3;
		frb_info(adev, "Found UVD firmware ENC: %u.%u DEC: .%u Family ID: %u",
			 enc_major, enc_minor, dec_minor, family_id);

		uvd->max_handles = AMDGPU_MAX_UVD_HANDLES;

		uvd->fw_version = uvd->ucode.ucode_version;
	}

	bo_size = AMDGPU_UVD_STACK_SIZE + AMDGPU_UVD_HEAP_SIZE
		  + AMDGPU_UVD_SESSION_SIZE * uvd->max_handles;
	/* the PSP's TMR holds the image on SOC15; on VI the driver loads it into the bo */
	if (!uvd->psp_load)
		bo_size += AMDGPU_GPU_PAGE_ALIGN(uvd->ucode.ucode_size_bytes + 8);

	/* amdgpu_bo_create_kernel(VRAM | GTT) per instance: the layout's UVD region, then the msg bo */
	need = 0;
	for (j = 0; j < uvd->num_uvd_inst; j++)
		if (!(uvd->harvest_config & (1 << j)))
			need += AMDGPU_GPU_PAGE_ALIGN(bo_size);
	if (!adev->layout.uvd_size ||
	    need + FRB_UVD_IB_BO_SIZE > adev->layout.uvd_size ||
	    !frb_vram_ok(adev, adev->layout.uvd, need + FRB_UVD_IB_BO_SIZE)) {
		frb_err(adev, "(%d) failed to allocate UVD bo (%u bytes, region %u at 0x%08x)", -ENOMEM,
			need + FRB_UVD_IB_BO_SIZE, adev->layout.uvd_size, adev->layout.uvd);
		return -ENOMEM;
	}
	uvd->vcpu_bo_size = bo_size;
	next = adev->layout.uvd;
	for (j = 0; j < uvd->num_uvd_inst; j++) {
		if (uvd->harvest_config & (1 << j))
			continue;
		uvd->inst[j].vcpu_bo = next;
		uvd->inst[j].gpu_addr = frb_vram_gpu_addr(adev, next);
		next += AMDGPU_GPU_PAGE_ALIGN(bo_size);
	}

	/* from uvd v5.0 HW addressing capacity increased to 64 bits (SI's 3.1, CIK's 4.2: 32 + segments) */
	uvd->address_64_bit = !frb_is_si(adev) && !frb_is_cik(adev);

	/*
	 * amdgpu_uvd_create_msg_bo_helper(128 KB): without address_64_bit (SI)
	 * the msg bo goes into the UVD segment (amdgpu_uvd_force_into_uvd_segment:
	 * VRAM's first 256 MB); right after the VCPU bo(s) it is there either way
	 */
	uvd->ib_bo = next;
	uvd->ib_bo_gpu_addr = frb_vram_gpu_addr(adev, uvd->ib_bo);

	/* default: CHIP_POLARIS10 and later */
	uvd->use_ctx_buf = adev->asic_type >= CHIP_POLARIS10;

	if ((adev->asic_type == CHIP_POLARIS10 || adev->asic_type == CHIP_POLARIS11) &&
	    uvd->fw_version < FW_1_66_16)
		frb_err(adev, "POLARIS10/11 UVD firmware version %u.%u is too old.",
			version_major, version_minor);

	for (j = 0; j < uvd->num_uvd_inst; j++) {
		if (uvd->harvest_config & (1 << j))
			continue;
		frb_info(adev, "uvd: instance %u: VCPU bo %u bytes at %llx (BAR0 0x%08x)", j, bo_size,
			 (unsigned long long)uvd->inst[j].gpu_addr, uvd->inst[j].vcpu_bo);
	}
	frb_info(adev, "uvd: msg bo at %llx; %u handles; firmware %s", (unsigned long long)uvd->ib_bo_gpu_addr,
		 uvd->max_handles, uvd->psp_load ? "in the PSP's TMR" : "loaded by the driver");
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
	uint32_t size = 0, ptr, offset, i, j, copied = 0;
	uint64_t t0 = adev->os->time_ms(adev->os->cookie);

	for (j = 0; j < uvd->num_uvd_inst; j++) {
		if (uvd->harvest_config & (1 << j))
			continue;
		if (!uvd->inst[j].vcpu_bo)
			return -EINVAL;

		size = uvd->vcpu_bo_size;
		ptr = uvd->inst[j].vcpu_bo;
		if (!uvd->psp_load) {
			offset = uvd->ucode.ucode_array_offset;
			for (i = 0; i < uvd->ucode.ucode_size_bytes; i += 4)
				frb_vram_write32(adev, ptr + i, frb_get_le32(uvd->fw.data, offset + i));
			size -= uvd->ucode.ucode_size_bytes;
			ptr += uvd->ucode.ucode_size_bytes;
			copied = uvd->ucode.ucode_size_bytes;
		}
		for (i = 0; i < size; i += 4)
			frb_vram_write32(adev, ptr + i, 0);
		/* the VCPU fetches it through the MC: the CPU's BAR0 writes out of the HDP first */
		frb_flush_hdp(adev);

		/* to restore uvd fence seq */
		if (uvd->inst[j].ring.funcs)
			frb_fence_driver_force_completion(&uvd->inst[j].ring);
	}
	if (uvd->psp_load)
		frb_info(adev, "uvd: VCPU bo(s) zeroed, %u bytes each (the firmware is the PSP's to load; %llu ms)",
			 size, (unsigned long long)(adev->os->time_ms(adev->os->cookie) - t0));
	else
		frb_info(adev, "uvd: firmware %u bytes into the VCPU bo, %u zeroed after it (%llu ms)",
			 copied, size, (unsigned long long)(adev->os->time_ms(adev->os->cookie) - t0));
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

	/* adev->reg_offset[UVD_HWIP][ring->me][1] from Vega 10 on, UVD_BASE_SI before */
	offset = frb_uvd_reg_base(adev, ring->me);

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
	uint32_t fences = 0, i, rings = 0;

	if (!uvd)
		return -ENODEV;
	for (i = 0; i < uvd->num_uvd_inst; ++i) {
		if ((uvd->harvest_config & (1 << i)) || !uvd->inst[i].ring.funcs)
			continue;
		rings++;
		/* amdgpu_fence_count_emitted */
		fences += uvd->inst[i].ring.sync_seq - frb_fence_read(&uvd->inst[i].ring);
	}
	if (!rings)
		return -ENODEV;
	if (fences)
		return -EBUSY;
	if (!uvd->powered)
		return 0;

	if (frb_is_soc15(adev)) {
		/*
		 * adev->pm.dpm_enabled: amdgpu_dpm_enable_uvd(false) ->
		 * vega20_power_gate_uvd(true): the SMU's DPM_UVD feature off. Else
		 * soc15_set_uvd_clocks(0, 0) (empty), no set_powergating_state on
		 * uvd_v7_0, an empty set_clockgating_state: nothing.
		 */
		if (adev->smu11 && adev->smu11->dpm_enabled)
			frb_vega20_power_gate_uvd(adev, 1);
	} else if (frb_pm_enabled(adev)) {
		frb_pm_enable_uvd(adev, 0);
	} else if (frb_is_si(adev)) {
		/* amdgpu_asic_set_uvd_clocks(0, 0): the UPLL in bypass; uvd_v3_1's gating calls are empty */
		frb_si_set_uvd_clocks(adev, 0, 0);
	} else if (frb_is_cik(adev)) {
		frb_cik_set_uvd_clocks(adev, 0, 0);
		/* shutdown the UVD block (uvd_v4_2_set_powergating_state GATE: the VCPU stopped) */
		frb_uvd_v4_2_set_powergating_state(adev, 1);
		frb_uvd_v4_2_set_clockgating_state(adev, 1);
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
	if (frb_is_soc15(adev)) {
		/* amdgpu_dpm_enable_uvd(true) -> vega20_power_gate_uvd(false); without DPM three no-ops */
		if (adev->smu11 && adev->smu11->dpm_enabled)
			frb_vega20_power_gate_uvd(adev, 0);
		return;
	}
	if (frb_pm_enabled(adev)) {
		/* smu7_powergate_uvd(false): ungated, the VCPU started, UVD's DPM levels on */
		frb_pm_enable_uvd(adev, 1);
		return;
	}
	if (frb_is_si(adev)) {
		/* si_set_uvd_clocks; uvd_v3_1's set_clockgating/powergating_state are empty */
		r = frb_si_set_uvd_clocks(adev, 53300, 40000);
		if (r)
			frb_err(adev, "uvd: si_set_uvd_clocks(533, 400 MHz) failed (%d)", r);
		return;
	}
	if (frb_is_cik(adev)) {
		r = frb_cik_set_uvd_clocks(adev, 53300, 40000);
		if (r)
			frb_err(adev, "uvd: cik_set_uvd_clocks(533, 400 MHz) failed (%d)", r);
		frb_uvd_v4_2_set_clockgating_state(adev, 0);
		r = frb_uvd_v4_2_set_powergating_state(adev, 0);
		if (r)
			frb_err(adev, "uvd: uvd_v4_2_start failed (%d)", r);
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
