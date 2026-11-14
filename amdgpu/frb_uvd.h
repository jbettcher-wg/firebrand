/*
 * amdgpu_uvd.h, the parts Firebrand has (Linux 7.0.2), and what the
 * accelerator's video sessions (kext/Firebrand/FirebrandUvdSession.cpp) use
 * of the UVD block: the decode ring, IBs written in the ring's byte order,
 * the power reference (amdgpu_uvd_ring_begin_use / the idle work).
 *
 * Only the decode ring of one UVD 6.3 instance (Polaris 12): no encode
 * rings (uvd_v6_0_enc_support is true for this firmware on Linux; they are
 * out of Firebrand's scope), no VCE, no interrupts (fences are polled).
 *
 * Copyright 2011 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_UVD_H
#define FRB_UVD_H

#include "frb_ring.h"
#include "frb_ucode.h"

#define AMDGPU_DEFAULT_UVD_HANDLES	10
#define AMDGPU_MAX_UVD_HANDLES		40
#define AMDGPU_UVD_STACK_SIZE		(200*1024)
#define AMDGPU_UVD_HEAP_SIZE		(256*1024)
#define AMDGPU_UVD_SESSION_SIZE		(50*1024)
#define AMDGPU_UVD_FIRMWARE_OFFSET	256

/* amdgpu_uvd.h AMDGPU_UVD_FIRMWARE_SIZE, from the header's ucode_size_bytes */
#define AMDGPU_GPU_PAGE_ALIGN(a)	(((a) + AMDGPU_GPU_PAGE_SIZE - 1) & ~(uint32_t)(AMDGPU_GPU_PAGE_SIZE - 1))
#define AMDGPU_UVD_FIRMWARE_SIZE(uvd)	\
	(AMDGPU_GPU_PAGE_ALIGN((uvd)->ucode.ucode_size_bytes + 8) - AMDGPU_UVD_FIRMWARE_OFFSET)

/* the kernel's msg bo (amdgpu: uvd.ib_bo, 128 KB in GTT; here VRAM after the VCPU bo) */
#define FRB_UVD_IB_BO_SIZE		(128u << 10)

/* amdgpu_uvd.c: the GPCOM registers relative to UVD_BASE_SI */
#define UVD_GPCOM_VCPU_CMD		0x03c3
#define UVD_GPCOM_VCPU_DATA0		0x03c4
#define UVD_GPCOM_VCPU_DATA1		0x03c5
#define UVD_NO_OP			0x03ff
#define UVD_BASE_SI			0x3800

struct frb_uvd {
	/* amdgpu_uvd */
	struct frb_fw fw;		/* polaris12_uvd.bin */
	struct frb_ucode ucode;		/* its common_firmware_header */
	uint32_t fw_version;		/* (major << 24) | (minor << 16) | (family << 8) */
	uint32_t max_handles;
	int address_64_bit;
	int use_ctx_buf;

	/* amdgpu_uvd_inst */
	uint32_t vcpu_bo;		/* BAR0 offset of the VCPU bo */
	uint32_t vcpu_bo_size;
	uint64_t gpu_addr;
	uint32_t ib_bo;			/* BAR0 offset of the msg bo */
	uint64_t ib_bo_gpu_addr;
	struct frb_ring ring;
	uint32_t srbm_soft_reset;

	/* Firebrand */
	int swapped;			/* LMI_SWAP_CNTL 0xa: ring and IBs in the host's order (amdgpu's
					   __BIG_ENDIAN arm); 0: little-endian, swap 0 (the default) */
	int powered;			/* begin_use started the VCPU and the idle work has not stopped it */
	int touched;			/* hw_init wrote the block: hw_fini must stop it */
	uint32_t ib_tests;		/* IB tests run (the msg handle cycles) */
	uint32_t starts;		/* uvd_v6_0_start calls that came up */
};

/* amdgpu_uvd.c */
int frb_uvd_sw_init(struct frb_device *adev);
int frb_uvd_resume(struct frb_device *adev);
/* the msg's IB is *ib: frb_ib_free it once *seq has signalled */
int frb_uvd_get_create_msg(struct frb_ring *ring, uint32_t handle, struct frb_ib *ib, uint32_t *seq);
int frb_uvd_get_destroy_msg(struct frb_ring *ring, uint32_t handle, struct frb_ib *ib, uint32_t *seq);
int frb_uvd_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us);
/*
 * amdgpu_uvd_ring_begin_use: the clocks up and the VCPU started when the
 * idle work has stopped it (or nothing started it yet). There is no
 * delayed work here: frb_uvd_idle_work_handler runs when a caller says
 * the ring is idle (the end of stage uvd, a session's end), as amdgpu's
 * does a second after the last submission.
 */
void frb_uvd_ring_begin_use(struct frb_ring *ring);
/* amdgpu_uvd_idle_work_handler: no fences outstanding -> clocks 0, VCPU stopped, gated */
int frb_uvd_idle_work_handler(struct frb_device *adev);

/* uvd_v6_0.c, for frb_uvd.c and the sessions */
struct frb_ring *frb_uvd_ring(struct frb_device *adev);	/* NULL unless the ring is up */
int frb_uvd_v6_0_set_powergating_state(struct frb_device *adev, int gate);
int frb_uvd_v6_0_set_clockgating_state(struct frb_device *adev, int gate);
/* amdgpu_ring_write for this ring: little-endian, or host order under LMI swap 0xa */
void frb_uvd_ring_write(struct frb_ring *ring, uint32_t v);
/* an IB dword in the same byte order (frb_ib_write is little-endian only) */
void frb_uvd_ib_write(struct frb_device *adev, struct frb_ib *ib, uint32_t v);
/* NO_OP pairs to the ring's 16-dword fetch size */
void frb_uvd_ib_pad(struct frb_device *adev, struct frb_ib *ib);
/* the UVD PACKET0 a register write takes in an IB or the ring */
#define FRB_UVD_PACKET0(reg)	((uint32_t)((reg) & 0xffff))

#endif
