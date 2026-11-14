/*
 * amdgpu_device.c, the parts Firebrand has: register access, the register
 * trace, amdgpu_device_program_register_sequence, need_post, and the init
 * order of amdgpu_device_init / amdgpu_device_ip_init cut into stages.
 *
 * The order below is the order of the E9173 capture
 * (a Linux amdgpu register trace of its init), and the trace replay
 * checks that the register accesses come out in that order with those
 * values. What amdgpu does in its other IP blocks' early_init (gfx GDS
 * sizes, UVD/VCE harvest fuses) is reads only and is left out.
 *
 * Copyright 2008 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
 * Copyright 2009 Jerome Glisse.
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
#include "frb_device.h"
#include "frb_pm.h"
#include "frb_display.h"
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/smu/smu_7_1_3_d.h"

/* --- the register trace --------------------------------------------------- */

/*
 * A polling loop (SMC_RESP_0 through MCLKDPM_Enable's 256 ms, for one) would
 * fill the buffer with one read: after two identical reads of a register in
 * a row, further ones are counted in trace_squashed, not stored.
 * frb-trace-compare.py collapses polling loops in both traces anyway.
 */
static inline void trace_add(struct frb_device *adev, uint32_t reg, uint32_t val)
{
	if (!adev->trace_on)
		return;
	if (!(reg & FRB_TRACE_WRITE) && adev->trace_len >= 2 &&
	    adev->trace[adev->trace_len - 1].reg == reg && adev->trace[adev->trace_len - 1].val == val &&
	    adev->trace[adev->trace_len - 2].reg == reg && adev->trace[adev->trace_len - 2].val == val) {
		adev->trace_squashed++;
		return;
	}
	if (adev->trace_len < adev->trace_cap) {
		adev->trace[adev->trace_len].reg = reg;
		adev->trace[adev->trace_len].val = val;
		adev->trace_len++;
	} else {
		adev->trace_lost++;
	}
}

void frb_trace_start(struct frb_device *adev)
{
	adev->trace_len = 0;
	adev->trace_lost = 0;
	adev->trace_squashed = 0;
	adev->trace_on = adev->trace && adev->trace_cap;
}

void frb_trace_stop(struct frb_device *adev)
{
	adev->trace_on = 0;
}

/* --- register access ------------------------------------------------------- */

/*
 * amdgpu_device_rreg/wreg: the register BAR directly below rmmio_size, the
 * PCIE index/data pair above it (vi_pcie_rreg via adev->pcie_rreg). Every
 * VI register the bring-up touches is below 256 KB.
 */
uint32_t frb_rreg(struct frb_device *adev, uint32_t reg)
{
	uint32_t v;

	if (reg * 4 < adev->rmmio_size) {
		v = adev->os->mmio_read32(adev->os->cookie, reg * 4);
		trace_add(adev, reg, v);
		return v;
	}
	return frb_pcie_rreg(adev, reg * 4);
}

void frb_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	if (reg * 4 < adev->rmmio_size) {
		trace_add(adev, reg | FRB_TRACE_WRITE, v);
		adev->os->mmio_write32(adev->os->cookie, reg * 4, v);
		return;
	}
	frb_pcie_wreg(adev, reg * 4, v);
}

/* vi.c vi_smc_rreg / vi_smc_wreg: the SMC window used by the driver (11) */
uint32_t frb_smc_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32_NO_KIQ(mmSMC_IND_INDEX_11, (reg));
	return RREG32_NO_KIQ(mmSMC_IND_DATA_11);
}

void frb_smc_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32_NO_KIQ(mmSMC_IND_INDEX_11, (reg));
	WREG32_NO_KIQ(mmSMC_IND_DATA_11, (v));
}

/* vi.c vi_pcie_rreg / vi_pcie_wreg (amdgpu_device_indirect_rreg's shape) */
uint32_t frb_pcie_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32_NO_KIQ(mmPCIE_INDEX, reg);
	(void)RREG32_NO_KIQ(mmPCIE_INDEX);
	return RREG32_NO_KIQ(mmPCIE_DATA);
}

void frb_pcie_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32_NO_KIQ(mmPCIE_INDEX, reg);
	(void)RREG32_NO_KIQ(mmPCIE_INDEX);
	WREG32_NO_KIQ(mmPCIE_DATA, v);
	(void)RREG32_NO_KIQ(mmPCIE_DATA);
}

/* amdgpu_device_program_register_sequence (VI: or_mask is not masked) */
void frb_device_program_register_sequence(struct frb_device *adev,
					  const uint32_t *registers, uint32_t array_size)
{
	uint32_t tmp, reg, and_mask, or_mask;
	uint32_t i;

	if (array_size % 3)
		return;

	for (i = 0; i < array_size; i += 3) {
		reg = registers[i + 0];
		and_mask = registers[i + 1];
		or_mask = registers[i + 2];

		if (and_mask == 0xffffffff) {
			tmp = or_mask;
		} else {
			tmp = RREG32(reg);
			tmp &= ~and_mask;
			tmp |= or_mask;
		}
		WREG32(reg, tmp);
	}
}

/* --- VRAM through BAR0 ----------------------------------------------------- */

/*
 * The CPU reaches VRAM only through BAR0, which on this platform is the
 * first 256 MB of VRAM (QorIQPCIBridge leaves it at 256 MB; the 32-bit
 * IOKit ABI rules out resizing it).
 * Words go out little-endian, the GPU's order, whatever the host is.
 */
int frb_vram_ok(struct frb_device *adev, uint32_t offset, uint32_t bytes)
{
	return adev->aper && !(offset & 3) && bytes <= adev->aper_size &&
	       offset <= adev->aper_size - bytes;
}

void frb_vram_write32(struct frb_device *adev, uint32_t offset, uint32_t v)
{
	*(volatile uint32_t *)(adev->aper + offset) = frb_swap_le32(v);
}

uint32_t frb_vram_read32(struct frb_device *adev, uint32_t offset)
{
	return frb_swap_le32(*(volatile uint32_t *)(adev->aper + offset));
}

/* --- need_post (amdgpu_device_need_post: CIK and later use the scratch) ---- */

int frb_device_need_post(struct frb_device *adev)
{
	if (adev->has_hw_reset) {
		adev->has_hw_reset = 0;
		return 1;
	}
	return frb_atombios_scratch_need_asic_init(adev);
}

const char *frb_stage_name(enum frb_stage stage)
{
	switch (stage) {
	case FRB_STAGE_SURVEY:	return "survey";
	case FRB_STAGE_POST:	return "post";
	case FRB_STAGE_GMC:	return "gmc";
	case FRB_STAGE_SMU:	return "smu";
	case FRB_STAGE_DCE:	return "dce";
	case FRB_STAGE_GFX:	return "gfx";
	case FRB_STAGE_SDMA:	return "sdma";
	case FRB_STAGE_UVD:	return "uvd";
	default:		return "?";
	}
}

/*
 * F1: what amdgpu_device_init does from the VBIOS to "GPU posting now":
 * amdgpu_atombios_init's scratch registers, vi_need_reset_on_init,
 * amdgpu_device_need_post, amdgpu_device_asic_init, then the clock info.
 */
static int stage_post(struct frb_device *adev)
{
	uint64_t t0;
	int r;

	frb_atombios_scratch_regs_init(adev);

	if (frb_vi_need_reset_on_init(adev)) {
		/*
		 * "check if we need to reset the asic, e.g. driver was not
		 * cleanly unloaded previously": nothing but Firebrand starts
		 * this SMC on these machines (a reboot's PERST stops it), so a
		 * running one is an earlier load's in this boot. amdgpu resets
		 * the ASIC here (vi_asic_reset), and so does Firebrand; the
		 * card then comes back unposted and every stage runs as on a
		 * cold card.
		 *
		 * With adopt that state is taken as it is instead (the display
		 * an earlier load lit stays lit until stage dce): the card is
		 * posted (need_post says so), the GMC stage reloads the MC
		 * microcode as on a fresh card, and polaris10_start_smu's own
		 * "SMC RAM already running" arm re-sends the microcode TOC.
		 */
		if (adev->adopt) {
			frb_info(adev, "post: ADOPT: the SMC is already running; continuing without the reset "
				 "amdgpu would do (FirebrandAdopt)");
		} else {
			frb_info(adev, "post: the SMC is already running (an earlier load in this boot): "
				 "resetting the ASIC, as amdgpu does on init");
			r = frb_vi_asic_reset(adev);
			if (r) {
				frb_err(adev, "post: asic reset on init failed (%d)", r);
				return r;
			}
		}
	}

	if (!frb_device_need_post(adev)) {
		frb_info(adev, "post: BIOS_SCRATCH_7 says ASIC_Init is complete; not posting");
	} else {
		frb_info(adev, "post: GPU posting now...");
		t0 = adev->os->time_ms(adev->os->cookie);
		r = frb_device_asic_init(adev);
		adev->asic_init_ms = (uint32_t)(adev->os->time_ms(adev->os->cookie) - t0);
		if (r) {
			frb_err(adev, "post: gpu post error! (%d, after %u ms)", r, adev->asic_init_ms);
			return r < 0 ? r : -EIO;
		}
		frb_info(adev, "post: ASIC_Init returned 0 after %u ms", adev->asic_init_ms);
	}
	r = frb_atombios_get_clock_info(adev);
	if (r)
		return r;
	return 0;
}

/*
 * F2: amdgpu_device_ip_init for COMMON and GMC, which it brings up first and
 * at once (sw_init then hw_init): vi_common_hw_init, gmc_v8_0_sw_init,
 * gmc_v8_0_hw_init.
 */
static int stage_gmc(struct frb_device *adev)
{
	int r;

	r = frb_vi_common_hw_init(adev);
	if (r)
		return r;
	frb_gmc_v8_0_early_init(adev);
	r = frb_gmc_v8_0_sw_init(adev);
	if (r)
		return r;
	return frb_gmc_v8_0_hw_init(adev);
}

/*
 * The SMC block's hw_init and late_init (DPM), with FirebrandDPM /
 * frb_dpm=1: after stage sdma, where amdgpu's IP-block order puts pp_smu
 * (frb_amd_powerplay.c says why not in stage smu), and so before stage uvd,
 * whose begin_use then takes powerplay's UVD arm as on Linux (the ring
 * test's UVDDPM_Enable). A failure is reported and the card stays at its
 * boot clocks.
 */
static void stage_dpm(struct frb_device *adev)
{
	int pr;

	if (!adev->want_dpm || adev->stage_done < FRB_STAGE_SDMA || adev->pm || adev->dpm_failed)
		return;
	frb_info(adev, "dpm: begin (powerplay hw_init, late_init)");
	pr = frb_pm_hw_init(adev);
	if (pr) {
		adev->dpm_failed = pr;
		frb_err(adev, "dpm: FAILED (%d); staying at the boot clocks", pr);
	} else {
		frb_info(adev, "dpm: done");
		/* a display stage dce lit: its watermarks and powerplay's display configuration */
		frb_display_dpm_changed(adev);
	}
}

int frb_device_init(struct frb_device *adev, enum frb_stage stage)
{
	enum frb_stage s;
	int r = 0;

	if (adev->usec_timeout == 0)
		adev->usec_timeout = AMDGPU_MAX_USEC_TIMEOUT;
	if (!adev->atom)
		return -ENODEV;
	if (stage > FRB_STAGE_SURVEY && !adev->row) {
		frb_err(adev, "no row in the family table for %04x rev %02x subsystem %04x:%04x; "
			"survey only", adev->device, adev->revision,
			adev->subsys_vendor, adev->subsys_device);
		return -EPERM;
	}
	if (adev->row && stage > adev->row->max_stage) {
		if (!adev->allow_unverified) {
			frb_err(adev, "stage %s asked, but %s is verified only to stage %s",
				frb_stage_name(stage), adev->row->board,
				frb_stage_name((enum frb_stage)adev->row->max_stage));
			return -EPERM;
		}
		frb_info(adev, "BRING-UP: stage %s on %s, verified only to stage %s (FirebrandBringUp)",
			 frb_stage_name(stage), adev->row->board,
			 frb_stage_name((enum frb_stage)adev->row->max_stage));
	}
	if (adev->stage_failed) {
		frb_err(adev, "an earlier stage failed (%d); reboot before trying again",
			adev->stage_failed);
		return adev->stage_failed;
	}

	for (s = (enum frb_stage)(adev->stage_done + 1); s <= stage && !r; s++) {
		frb_info(adev, "stage %s: begin", frb_stage_name(s));
		switch (s) {
		case FRB_STAGE_POST:	r = stage_post(adev);		break;
		case FRB_STAGE_GMC:	r = stage_gmc(adev);		break;
		case FRB_STAGE_SMU:	r = frb_smu7_start(adev);	break;
		case FRB_STAGE_DCE:	r = frb_display_init(adev);	break;
		case FRB_STAGE_GFX:	r = frb_gfx_v8_0_init(adev);	break;
		case FRB_STAGE_SDMA:	r = frb_sdma_v3_0_init(adev);	break;
		case FRB_STAGE_UVD:	r = frb_uvd_v6_0_init(adev);	break;
		default:		r = -EINVAL;			break;
		}
		if (r) {
			adev->stage_failed = r;
			frb_err(adev, "stage %s: FAILED (%d)", frb_stage_name(s), r);
		} else {
			adev->stage_done = s;
			frb_info(adev, "stage %s: done", frb_stage_name(s));
			if (s == FRB_STAGE_SDMA)
				stage_dpm(adev);
		}
	}

	if (!r)
		stage_dpm(adev);
	return r;
}
