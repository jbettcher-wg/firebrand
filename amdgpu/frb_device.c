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
#include "frb_soc15.h"
#include "frb_psp.h"
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

/* adev->smc_rreg / smc_wreg: the family's SMC window (VI: vi_smc_rreg's 11, SI: si_smc_rreg's 0) */
uint32_t frb_smc_rreg(struct frb_device *adev, uint32_t reg)
{
	return frb_funcs(adev)->smc_rreg(adev, reg);
}

void frb_smc_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	frb_funcs(adev)->smc_wreg(adev, reg, v);
}

/* vi.c vi_smc_rreg / vi_smc_wreg: the SMC window used by the driver (11) */
uint32_t frb_vi_smc_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32_NO_KIQ(mmSMC_IND_INDEX_11, (reg));
	return RREG32_NO_KIQ(mmSMC_IND_DATA_11);
}

void frb_vi_smc_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32_NO_KIQ(mmSMC_IND_INDEX_11, (reg));
	WREG32_NO_KIQ(mmSMC_IND_DATA_11, (v));
}

/* vi.c vi_pcie_rreg / vi_pcie_wreg (amdgpu_device_indirect_rreg's shape) */
uint32_t frb_pcie_rreg(struct frb_device *adev, uint32_t reg)
{
	uint32_t index = frb_funcs(adev)->pcie_index, data = frb_funcs(adev)->pcie_data;

	WREG32_NO_KIQ(index, reg);
	(void)RREG32_NO_KIQ(index);
	return RREG32_NO_KIQ(data);
}

void frb_pcie_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	uint32_t index = frb_funcs(adev)->pcie_index, data = frb_funcs(adev)->pcie_data;

	WREG32_NO_KIQ(index, reg);
	(void)RREG32_NO_KIQ(index);
	WREG32_NO_KIQ(data, v);
	(void)RREG32_NO_KIQ(data);
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
	uint32_t reg;

	if (adev->has_hw_reset) {
		adev->has_hw_reset = 0;
		return 1;
	}
	/* bios scratch used on CIK+ */
	if (frb_funcs(adev)->family != AMDGPU_FAMILY_SI)
		return frb_atombios_scratch_need_asic_init(adev);
	/*
	 * check MEM_SIZE for older asics: SI's VBIOS leaves BIOS_SCRATCH_7's
	 * ASIC_Init bit clear (measured on the R7 450: 0 after a good ASIC_Init)
	 */
	reg = frb_funcs(adev)->get_config_memsize(adev);
	if ((reg != 0) && (reg != 0xffffffff))
		return 0;
	return 1;
}

/* the family's early_init: reads only (the revision) */
void frb_device_early_init(struct frb_device *adev)
{
	frb_funcs(adev)->early_init(adev);
}

/* the survey: VBIOS identity and tables, the card's state, by reads only */
void frb_device_survey(struct frb_device *adev)
{
	if (adev->is_atom_fw)
		frb_atomfirmware_survey(adev);
	else
		frb_atombios_survey(adev);
}

uint32_t frb_device_get_config_memsize(struct frb_device *adev)
{
	return frb_funcs(adev)->get_config_memsize(adev);
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

	if (frb_funcs(adev)->need_reset_on_init && frb_funcs(adev)->need_reset_on_init(adev)) {
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
			frb_info(adev, "post: ADOPT: an earlier load left the card running (%s); continuing without "
				 "the reset (FirebrandAdopt)", frb_funcs(adev)->name);
		} else {
			frb_info(adev, "post: an earlier load in this boot left the card running (%s: VI's SMC, SI's CP, "
				 "CIK's SMC, CP or moved VRAM): resetting the ASIC, as amdgpu does on init",
				 frb_funcs(adev)->name);
			r = frb_funcs(adev)->asic_reset ? frb_funcs(adev)->asic_reset(adev) : -ENOSYS;
			if (r) {
				frb_err(adev, "post: asic reset on init failed (%d)", r);
				return r;
			}
		}
	}

	if (!frb_device_need_post(adev)) {
		frb_info(adev, "post: %s says the card is posted; not posting",
			 frb_funcs(adev)->family == AMDGPU_FAMILY_SI ? "CONFIG_MEMSIZE" : "BIOS_SCRATCH_7");
	} else {
		frb_info(adev, "post: GPU posting now...");
		/* amdgpu_device_asic_init: amdgpu_asic_pre_asic_init first (CIK: the SMC's clock, frb_cik.c) */
		if (frb_funcs(adev)->pre_asic_init)
			frb_funcs(adev)->pre_asic_init(adev);
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
 * F1 on SOC15 (Vega 20), the same steps of amdgpu_device_init:
 * amdgpu_atomfirmware_scratch_regs_init has no register writes (it ran in
 * frb_atombios_init); soc15_need_reset_on_init is false on bare metal, so
 * amdgpu never resets a Vega at load, and nor does Firebrand (a sOS an
 * earlier load started stays: psp_v11_0's bootloader steps skip themselves
 * when it answers, stage smu); need_post reads BIOS_SCRATCH_7 as on VI;
 * amdgpu_device_asic_init takes amdgpu_atom_asic_init for GC 9.4.0
 * (soc15_pre_asic_init restores only DCN registers); then
 * amdgpu_atomfirmware_get_clock_info.
 */
static int stage_post_soc15(struct frb_device *adev)
{
	uint64_t t0;
	int r;

	if (frb_psp_v11_0_sos_alive(adev))
		frb_info(adev, "post: the PSP's sOS is already running (an earlier load in this boot); amdgpu does "
			 "not reset a SOC15 card at load (soc15_need_reset_on_init), nor does Firebrand");
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
	return frb_atomfirmware_get_clock_info(adev);
}

/*
 * F2 on SOC15: COMMON's hw_init (soc15_common_hw_init), then GMC's sw_init
 * and hw_init (gmc_v9_0, both VM hubs), which amdgpu_device_ip_init runs
 * early and at once.
 */
static int stage_gmc_soc15(struct frb_device *adev)
{
	int r;

	r = frb_soc15_common_hw_init(adev);
	if (r)
		return r;
	r = frb_gmc_v9_0_sw_init(adev);
	if (r)
		return r;
	return frb_gmc_v9_0_hw_init(adev);
}

/* the stages SOC15 has (gfx v9, sdma v4 after DCE 12, then uvd v7; VCE is not ported) */
static int stage_soc15(struct frb_device *adev, enum frb_stage s)
{
	switch (s) {
	case FRB_STAGE_POST:	return stage_post_soc15(adev);
	case FRB_STAGE_GMC:	return stage_gmc_soc15(adev);
	case FRB_STAGE_SMU:	return frb_soc15_stage_smu(adev);
	case FRB_STAGE_DCE:	return frb_display_init(adev);
	case FRB_STAGE_GFX:	return frb_gfx_v9_0_init(adev);
	case FRB_STAGE_SDMA:	return frb_sdma_v4_0_init(adev);
	case FRB_STAGE_UVD:	return frb_uvd_v7_0_init(adev);
	default:
		frb_err(adev, "stage %s is not ported to %s yet", frb_stage_name(s), frb_asic_name(adev->asic_type));
		return -ENOSYS;
	}
}

/*
 * amdgpu_device_set_cg_state on SOC15 (Vega 20): every block's
 * set_clockgating_state, in IP-block order to gate (soc15_common, gmc_v9_0;
 * vega20_ih, psp, powerplay and DM have none that writes; gfx_v9_0,
 * sdma_v4_0; amdgpu's late init skips UVD and VCE, and uvd_v7_0's
 * set_clockgating_state is empty) and in reverse to
 * ungate. Only blocks that are up take part. amdgpu_device_set_pg_state
 * writes nothing on Vega 20 (pg_flags 0; gfx_v9_0 and sdma_v4_0 gate power
 * only on the APUs and SDMA 4.1).
 */
int frb_soc15_set_cg_state(struct frb_device *adev, int gate)
{
	if (!frb_is_soc15(adev))
		return -ENODEV;
	if (adev->stage_done < FRB_STAGE_GMC)
		return -EPERM;
	if (gate) {
		frb_soc15_common_set_clockgating_state(adev, 1);
		frb_gmc_v9_0_set_clockgating_state(adev, 1);
		frb_gfx_v9_0_set_clockgating_state(adev, 1);
		frb_sdma_v4_0_set_clockgating_state(adev, 1);
	} else {
		frb_sdma_v4_0_set_clockgating_state(adev, 0);
		frb_gfx_v9_0_set_clockgating_state(adev, 0);
		frb_gmc_v9_0_set_clockgating_state(adev, 0);
		frb_soc15_common_set_clockgating_state(adev, 0);
	}
	adev->cg_gated = gate;
	return 0;
}

/*
 * amdgpu_device_ip_late_init on SOC15, the parts with register writes
 * Firebrand carries: sdma_v4_0_late_init (ULV), then set_cg_state(GATE)
 * (FirebrandGating; the replays and the default have it on). It runs once
 * every ported block is up (stage sdma), as amdgpu's late_init follows every
 * hw_init. Not carried: the interrupt sources the late inits turn on (no
 * IH), RAS's error counters, KFD's debug trap set-up, the GDS EDC workaround.
 */
static void soc15_late_init(struct frb_device *adev)
{
	if (!adev->sdma_late_done && frb_sdma_v4_0_up(adev)) {
		frb_sdma_v4_0_late_init(adev);
		adev->sdma_late_done = 1;
	}
	if (!adev->want_gating) {
		frb_info(adev, "late init: clock gating off (FirebrandGating false / frb_cg=0)");
		return;
	}
	frb_soc15_set_cg_state(adev, 1);
	frb_info(adev, "late init: clock gating on (cg_flags %08x%08x: GFX MGCG/MGLS/CGCG/CGLS/3D, CP LS, MC, SDMA, "
		 "BIF LS, HDP LS, ROM); no power gating on Vega 20", (uint32_t)(adev->cg_flags >> 32),
		 (uint32_t)adev->cg_flags);
}

/*
 * F2: amdgpu_device_ip_init for COMMON and GMC, which it brings up first and
 * at once (sw_init then hw_init): vi_common_hw_init, gmc_v8_0_sw_init,
 * gmc_v8_0_hw_init (SI: si_common_hw_init, gmc_v6_0).
 */
static int stage_gmc(struct frb_device *adev)
{
	const struct frb_asic_funcs *f = frb_funcs(adev);
	int r;

	r = f->common_hw_init(adev);
	if (r)
		return r;
	if (f->gmc_early_init)
		f->gmc_early_init(adev);
	r = f->gmc_sw_init(adev);
	if (r)
		return r;
	return f->gmc_hw_init(adev);
}

/* a stage whose block the family has not ported: refused, never half run */
static int stage_call(struct frb_device *adev, int (*fn)(struct frb_device *), enum frb_stage s)
{
	if (!fn) {
		frb_err(adev, "stage %s: no %s port of this block yet", frb_stage_name(s), frb_funcs(adev)->name);
		return -ENOSYS;
	}
	return fn(adev);
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

	/* CIK: right after stage smu (frb_ci_dpm.c: a Hawaii heats up at its boot clocks) */
	if (!adev->want_dpm || adev->stage_done < (frb_is_cik(adev) ? FRB_STAGE_SMU : FRB_STAGE_SDMA) ||
	    adev->pm || adev->dpm_failed || (frb_is_cik(adev) && frb_pm_enabled(adev)))
		return;
	frb_info(adev, "dpm: begin (powerplay hw_init, late_init)");
	pr = frb_pm_hw_init(adev);
	if (pr == -ENOSYS) {
		adev->dpm_failed = pr;	/* not ported for this family (frb_pm_hw_init said which) */
	} else if (pr) {
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
	if (adev->row && stage > frb_stage_cap(adev)) {
		if (adev->row->untested && !frb_asic_ported(adev->asic_type)) {
			/* nothing of this chip is ported: not even FirebrandBringUp runs a stage on it */
			frb_err(adev, "stage %s asked, but %s has no Firebrand port yet", frb_stage_name(stage),
				frb_asic_name(adev->asic_type));
			return -ENOSYS;
		}
		if (!adev->allow_unverified) {
			if (adev->row->untested)
				frb_err(adev, "stage %s asked, but this is an untested card: display-only (stage %s) "
					"until it is unlocked (sudo frbctl unlock)", frb_stage_name(stage),
					frb_stage_name(frb_stage_cap(adev)));
			else
				frb_err(adev, "stage %s asked, but %s is verified only to stage %s",
					frb_stage_name(stage), adev->row->board,
					frb_stage_name((enum frb_stage)adev->row->max_stage));
			return -EPERM;
		}
		frb_info(adev, "BRING-UP: stage %s on %s, allowed only to stage %s (FirebrandBringUp)",
			 frb_stage_name(stage), adev->row->board, frb_stage_name(frb_stage_cap(adev)));
	}
	if (adev->stage_failed) {
		frb_err(adev, "an earlier stage failed (%d); reboot before trying again",
			adev->stage_failed);
		return adev->stage_failed;
	}

	/* hw_init runs ungated, as on Linux (late_init gates after it); gated blocks are ungated first */
	if (frb_is_soc15(adev) && adev->cg_gated && stage > adev->stage_done)
		frb_soc15_set_cg_state(adev, 0);

	for (s = (enum frb_stage)(adev->stage_done + 1); s <= stage && !r; s++) {
		frb_info(adev, "stage %s: begin", frb_stage_name(s));
		if (frb_is_soc15(adev)) {
			r = stage_soc15(adev, s);
			goto done;
		}
		switch (s) {
		case FRB_STAGE_POST:	r = stage_post(adev);		break;
		case FRB_STAGE_GMC:	r = stage_gmc(adev);		break;
		case FRB_STAGE_SMU:	r = stage_call(adev, frb_funcs(adev)->smu_start, s);	break;
		case FRB_STAGE_DCE:	r = frb_display_init(adev);				break;
		case FRB_STAGE_GFX:	r = stage_call(adev, frb_funcs(adev)->gfx_init, s);	break;
		case FRB_STAGE_SDMA:	r = stage_call(adev, frb_funcs(adev)->sdma_init, s);	break;
		case FRB_STAGE_UVD:	r = stage_call(adev, frb_funcs(adev)->uvd_init, s);	break;
		default:		r = -EINVAL;			break;
		}
done:
		if (r) {
			adev->stage_failed = r;
			adev->stage_failed_at = s;
			frb_err(adev, "stage %s: FAILED (%d)", frb_stage_name(s), r);
		} else {
			adev->stage_done = s;
			frb_info(adev, "stage %s: done", frb_stage_name(s));
			if ((s == FRB_STAGE_SDMA || (s == FRB_STAGE_SMU && frb_is_cik(adev))) && !frb_is_soc15(adev))
				stage_dpm(adev);
		}
	}

	if (!r && !frb_is_soc15(adev))
		stage_dpm(adev);
	if (!r && frb_is_soc15(adev) && adev->stage_done >= FRB_STAGE_SDMA)
		soc15_late_init(adev);
	return r;
}
