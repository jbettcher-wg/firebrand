/*
 * Southern Islands DPM: radeon's si_dpm.c as RadeonNI ported and verified it
 * (amdgpu/si_dpm/, carried unchanged from RadeonNI's hw at 172477c01:
 * on the R7 450 its SMC SRAM tables equalled Linux radeon's byte for byte,
 * the levels 300/150 .. 925/1125 MHz at 950 .. 1225 mV as Linux adjusts
 * them, a 10-minute idle/load cycle switching both ways). amdgpu's
 * pm/legacy-dpm/si_dpm.c is a mechanical port of the same code.
 *
 * This file is the frb_pm.h interface's SI arm over it: the card handed to
 * that code (si_dpm/rdn_card.h, Firebrand's OS layer, ATOM interpreter and
 * registers), the SMC image from the kext (verde_k_smc.bin for this 682b rev
 * 87: rdn_si_dpm_wants_kicker_smc carries Linux's list), and the display
 * configuration the DCE gives (frb_pm_compute_clocks) as radeon's
 * radeon_pm_compute_clocks would.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_device.h"
#include "frb_pm.h"
#include "frb_display.h"
#include "si_dpm/rdn_si_dpm.h"

/*
 * The state switch on record (RDN_SI_DPM_SWITCH_TRACE): while the SMC has
 * SwitchToSwState pending the hook samples the registers the switch moves
 * (the SMC's answer and level, the engine PLL, the memory PLL and DLLs, the
 * arbiter, the VDDC pins, the display gap, the RLC's and the BIF's request
 * lines) and keeps each change with the wait loop's count, and for the ones
 * that never stand still (the SMC's program counter, the CRTC's position)
 * what they ranged over. One switch that hangs then says how far it got.
 */
#define SW_EVENTS	96
#define SW_PCS		12

struct si_sw_reg {
	uint16_t off;		/* radeon's byte offset */
	const char *name;
};

static const struct si_sw_reg sw_regs[] = {
	{ 0x0230, "SMC_RESP" }, { 0x0798, "PROFILE_INDEX" },
	{ 0x0600, "SPLL_FUNC" }, { 0x0604, "SPLL_FUNC_2" }, { 0x0608, "SPLL_FUNC_3" }, { 0x0614, "SPLL_STATUS" },
	{ 0x0780, "GENERAL_PWRMGT" }, { 0x0788, "SCLK_PWRMGT" },
	{ 0x2ba0, "MCLK_PWRMGT" }, { 0x2ba4, "DLL_CNTL" }, { 0x2bb4, "MPLL_FUNC" }, { 0x2bb8, "MPLL_FUNC_1" },
	{ 0x2bbc, "MPLL_FUNC_2" }, { 0x2bc0, "MPLL_AD" }, { 0x2bc4, "MPLL_DQ" },
	{ 0x27e8, "MC_ARB_CG" }, { 0x25bc, "MC_CG_CONFIG" }, { 0x2774, "ARB_DRAM_TIMING" }, { 0x2808, "ARB_BURST_TIME" },
	{ 0x28c8, "MC_SEQ_SUP_CNTL" }, { 0x28e8, "TRAIN_WAKEUP" },
	{ 0x17a4, "GPIOPAD_Y" }, { 0x0828, "DISPLAY_GAP" }, { 0x07c4, "RLC_REQ_RSP" }, { 0x07f4, "BIF_REQ_RSP" },
	{ 0x0878, "ULV_CONTROL" }, { 0x6e70, "CRTC_CONTROL" },
};
#define SW_NREGS	(sizeof(sw_regs) / sizeof(sw_regs[0]))

/* read once before and once after: what the switch leans on */
static const struct si_sw_reg sw_once[] = {
	{ 0x04fc, "DISP_SLOW_SELECT" }, { 0x1734, "BIOS_SCRATCH_4" }, { 0x1798, "GPIOPAD_MASK" }, { 0x179c, "GPIOPAD_A" },
	{ 0x17a0, "GPIOPAD_EN" }, { 0x0714, "THERMAL_STATUS" }, { 0x087c, "ULV_PARAMETER" }, { 0x0660, "CLKPIN_CNTL" },
	{ 0x0664, "CLKPIN_CNTL_2" }, { 0x0618, "SPLL_CNTL_MODE" }, { 0x2760, "ARB_RAMCFG" }, { 0x2778, "ARB_DRAM_TIMING2" },
	{ 0x27f0, "ARB_DRAM_TIMING_1" }, { 0x27fc, "ARB_DRAM_TIMING2_1" }, { 0x2a00, "MC_SEQ_MISC0" },
	{ 0x0ca0, "DMIF_BUFFER" }, { 0x6b0c, "LB_MEMORY_SPLIT" }, { 0x6b18, "PRIORITY_A" }, { 0x6b1c, "PRIORITY_B" },
	{ 0x6cc8, "DPG_ARB3" }, { 0x6ccc, "DPG_URGENCY" }, { 0x6cd4, "DPG_STUTTER" }, { 0x6b40, "LB_INT_MASK" },
	{ 0x6b08, "VLINE_START_END" }, { 0x6e34, "V_BLANK_START_END" }, { 0x6e1c, "V_TOTAL" }, { 0x6e8c, "CRTC_STATUS" },
	{ 0x6ed4, "CRTC_UPDATE_LOCK" }, { 0x6ef4, "MASTER_UPDATE_LOCK" }, { 0x6ef8, "MASTER_UPDATE_MODE" },
	{ 0x6844, "GRPH_UPDATE" }, { 0x69b8, "CUR_UPDATE" }, { 0xc300, "RLC_CNTL" }, { 0xc34c, "RLC_STAT" },
	{ 0xc404, "RLC_CGCG_CGLS" }, { 0x8010, "GRBM_STATUS" }, { 0x0e50, "SRBM_STATUS" },
};
#define SW_NONCE	(sizeof(sw_once) / sizeof(sw_once[0]))

struct si_sw_event {
	uint32_t count;		/* the wait loop's count: a microsecond or more each */
	uint16_t ms;		/* since the message */
	uint8_t reg;
	uint32_t from, to;
};

struct si_sw_trace {
	int active;			/* a SwitchToSwState is pending */
	uint64_t t0;
	uint32_t last[SW_NREGS];
	struct si_sw_event ev[SW_EVENTS];
	uint32_t nev, lost, samples;
	uint32_t pc[SW_PCS], npc, pc_last, pc_changes;
	uint32_t vblank_edges, in_vblank, frame0, frame1, pos_min, pos_max;
};

struct frb_si_dpm {
	struct rdn_card card;
	struct rdn_chip_info chip;
	struct rdn_si_dpm *dpm;
	int forced;			/* enum amd_dpm_forced_level in force */
	struct amd_pp_display_configuration cfg;
	int have_cfg;
	int uvd_active;			/* amdgpu's adev->pm.dpm.uvd_active: the UVD state is the one asked for */
	struct frb_device *adev;
	struct si_sw_trace sw;
};

#define SI_SMC_PC_C		0x80000370
#define SI_SMC_IND_INDEX_0	0x200
#define SI_SMC_IND_DATA_0	0x204
#define SI_CRTC_STATUS		0x6e8c
#define SI_CRTC_STATUS_POSITION	0x6e90
#define SI_CRTC_STATUS_FRAME	0x6e98
#define SI_PPSMC_SwitchToSwState	0x20

static uint32_t sw_rd(struct frb_device *adev, uint32_t off)
{
	return frb_rreg(adev, off >> 2);
}

/* the SMC's program counter: its index register is free while a message is pending (nothing else holds the device) */
static uint32_t sw_smc_pc(struct frb_device *adev)
{
	frb_wreg(adev, SI_SMC_IND_INDEX_0 >> 2, SI_SMC_PC_C);
	return frb_rreg(adev, SI_SMC_IND_DATA_0 >> 2);
}

static void sw_log_regs(struct frb_device *adev, const char *what, const struct si_sw_reg *regs, uint32_t n,
			const uint32_t *vals)
{
	uint32_t i, k, v[4];
	const char *nm[4];

	/* four to a line; the kext has no snprintf */
	for (i = 0; i < n; i += 4) {
		for (k = 0; k < 4; k++) {
			nm[k] = i + k < n ? regs[i + k].name : "-";
			v[k] = i + k < n ? (vals ? vals[i + k] : sw_rd(adev, regs[i + k].off)) : 0;
		}
		frb_info(adev, "si_dpm: %s: %s %08x, %s %08x, %s %08x, %s %08x", what, nm[0], v[0], nm[1], v[1],
			 nm[2], v[2], nm[3], v[3]);
	}
}

static void sw_sample(struct frb_si_dpm *s, uint32_t count)
{
	struct frb_device *adev = s->adev;
	struct si_sw_trace *t = &s->sw;
	uint32_t i, v, pc;

	t->samples++;
	for (i = 0; i < SW_NREGS; i++) {
		v = sw_rd(adev, sw_regs[i].off);
		if (v == t->last[i])
			continue;
		if (t->nev < SW_EVENTS) {
			struct si_sw_event *e = &t->ev[t->nev++];

			e->count = count;
			e->ms = (uint16_t)(adev->os->time_ms(adev->os->cookie) - t->t0);
			e->reg = (uint8_t)i;
			e->from = t->last[i];
			e->to = v;
		} else {
			t->lost++;
		}
		t->last[i] = v;
	}
	pc = sw_smc_pc(adev);
	if (pc != t->pc_last)
		t->pc_changes++;
	t->pc_last = pc;
	for (i = 0; i < t->npc && t->pc[i] != pc; i++)
		;
	if (i == t->npc && t->npc < SW_PCS)
		t->pc[t->npc++] = pc;
	v = sw_rd(adev, SI_CRTC_STATUS) & 1;	/* V_BLANK */
	if (v && !t->in_vblank)
		t->vblank_edges++;
	t->in_vblank = v;
	v = sw_rd(adev, SI_CRTC_STATUS_POSITION) & 0x1fff;
	if (v < t->pos_min)
		t->pos_min = v;
	if (v > t->pos_max)
		t->pos_max = v;
}

static void si_dpm_msg_hook(void *ctx, uint32_t msg, int phase, uint32_t v)
{
	struct frb_si_dpm *s = (struct frb_si_dpm *)ctx;
	struct frb_device *adev = s->adev;
	struct si_sw_trace *t = &s->sw;
	uint32_t i, elapsed;

	if (msg != SI_PPSMC_SwitchToSwState || !(adev->si_dpm_quirks & RDN_SI_DPM_SWITCH_TRACE))
		return;
	switch (phase) {
	case 0:
		memset(t, 0, sizeof(*t));
		sw_log_regs(adev, "before the switch", sw_once, SW_NONCE, NULL);
		for (i = 0; i < SW_NREGS; i++)
			t->last[i] = sw_rd(adev, sw_regs[i].off);
		sw_log_regs(adev, "before the switch", sw_regs, SW_NREGS, t->last);
		t->pc_last = sw_smc_pc(adev);
		t->pos_min = 0xffffffff;
		t->frame0 = sw_rd(adev, SI_CRTC_STATUS_FRAME);
		t->in_vblank = sw_rd(adev, SI_CRTC_STATUS) & 1;
		frb_info(adev, "si_dpm: before the switch: SMC pc %08x, frame %u, line %u, PCIe LC_SPEED_CNTL %08x "
			 "LC_LINK_WIDTH_CNTL %08x", t->pc_last, t->frame0, sw_rd(adev, SI_CRTC_STATUS_POSITION) & 0x1fff,
			 frb_si_pciep_rreg(adev, 0xa4), frb_si_pciep_rreg(adev, 0xa2));
		t->t0 = adev->os->time_ms(adev->os->cookie);
		t->active = 1;
		break;
	case 1:
		if (t->active)
			sw_sample(s, v);
		break;
	case 2:
		if (!t->active)
			break;
		t->active = 0;
		elapsed = (uint32_t)(adev->os->time_ms(adev->os->cookie) - t->t0);
		sw_sample(s, 0xffffffff);
		t->frame1 = sw_rd(adev, SI_CRTC_STATUS_FRAME);
		frb_info(adev, "si_dpm: the switch: answer %u after %u ms, %u samples, %u changes (%u more not kept); "
			 "%u vblanks seen, frames %u -> %u, lines %u..%u", v, elapsed, t->samples, t->nev, t->lost,
			 t->vblank_edges, t->frame0, t->frame1, t->pos_min, t->pos_max);
		for (i = 0; i < t->nev; i++)
			frb_info(adev, "si_dpm:   +%u ms (count %u): %s %08x -> %08x", t->ev[i].ms, t->ev[i].count,
				 sw_regs[t->ev[i].reg].name, t->ev[i].from, t->ev[i].to);
		frb_info(adev, "si_dpm:   SMC pc: %u changes over the samples, last %08x; %u distinct%s: %08x %08x %08x %08x "
			 "%08x %08x %08x %08x %08x %08x %08x %08x", t->pc_changes, t->pc_last, t->npc,
			 t->npc == SW_PCS ? " or more" : "", t->pc[0], t->pc[1], t->pc[2], t->pc[3], t->pc[4], t->pc[5],
			 t->pc[6], t->pc[7], t->pc[8], t->pc[9], t->pc[10], t->pc[11]);
		if (v != 1) {
			sw_log_regs(adev, "after the unanswered switch", sw_regs, SW_NREGS, t->last);
			sw_log_regs(adev, "after the unanswered switch", sw_once, SW_NONCE, NULL);
			frb_info(adev, "si_dpm: after the unanswered switch: PCIe LC_SPEED_CNTL %08x LC_LINK_WIDTH_CNTL %08x",
				 frb_si_pciep_rreg(adev, 0xa4), frb_si_pciep_rreg(adev, 0xa2));
		}
		break;
	}
}

static int si_dpm_apply(struct frb_si_dpm *s, const struct amd_pp_display_configuration *cfg, const char *when);

static int chip_of(struct frb_device *adev, struct rdn_chip_info *chip)
{
	memset(chip, 0, sizeof(*chip));
	chip->family = RDN_FAMILY_SI;
	chip->dce = 6;
	switch (adev->asic_type) {
	case CHIP_TAHITI:
		chip->chip = RDN_CHIP_TAHITI;
		chip->name = "TAHITI";
		break;
	case CHIP_PITCAIRN:
		chip->chip = RDN_CHIP_PITCAIRN;
		chip->name = "PITCAIRN";
		break;
	case CHIP_VERDE:
		chip->chip = RDN_CHIP_VERDE;
		chip->name = "VERDE";
		break;
	default:
		return -ENODEV;	/* Oland and Hainan: si_dpm.c has rows, the port has no chip ids for them */
	}
	return 0;
}

/*
 * The system's PCIe speeds for si_dpm (radeon's rdev->pm.dpm.sys_pcie_mask
 * from drm_pcie_get_speed_cap_mask): the root port above the card. On the
 * T4240 boards it is gen 2 (QorIQ's PCIe controller); RadeonNI passed the
 * same constant.
 */
#define SI_SYS_PCIE_MASK	(RDN_SI_PCIE_SPEED_25 | RDN_SI_PCIE_SPEED_50)

static int si_dpm_hw_init(struct frb_device *adev);

/*
 * amdgpu's order: si_dpm's hw_init (the SMC's start and the first state
 * switch) runs in amdgpu_device_ip_init, the blocks' clock gating only in
 * amdgpu_device_ip_late_init, after it. Firebrand gates at the end of stage
 * sdma and starts DPM later, so the gating is taken off for the start and
 * put back after. FRB_SI_DPM_CG_KEEP leaves it on throughout (what the
 * attempts up to 2026-11-15 10:31 ran); FRB_SI_DPM_MC_UNGATED keeps the
 * MC's MGCG and light sleep off afterwards.
 */
int frb_si_dpm_hw_init(struct frb_device *adev)
{
	int r, regate = 0;

	if (adev->si_dpm)
		return 0;
	if (adev->si_dpm_wanted && adev->cg_on && !(adev->si_dpm_quirks & FRB_SI_DPM_CG_KEEP)) {
		frb_si_dma_set_clockgating_state(adev, 0);
		frb_gfx_v6_0_set_clockgating_state(adev, 0);
		frb_gmc_v6_0_set_clockgating_state(adev, 0);
		regate = 1;
		frb_info(adev, "si_dpm: clock gating off for the SMC's start and first switch (amdgpu gates after it)");
	}
	r = si_dpm_hw_init(adev);
	if (regate) {
		frb_gmc_v6_0_set_clockgating_state(adev, 1);
		frb_gfx_v6_0_set_clockgating_state(adev, 1);
		frb_si_dma_set_clockgating_state(adev, 1);
	}
	if (adev->cg_on && adev->si_dpm && (adev->si_dpm_quirks & FRB_SI_DPM_MC_UNGATED)) {
		frb_gmc_v6_0_set_mc_clockgating(adev, 0);
		frb_info(adev, "si_dpm: the MC's clock gating and light sleep stay off (FRB_SI_DPM_MC_UNGATED)");
	}
	return r;
}

static int si_dpm_hw_init(struct frb_device *adev)
{
	struct frb_si_dpm *s;
	const uint8_t *fw = 0;
	uint32_t fw_size = 0;
	const char *name;
	char other[32];
	unsigned level = 99;
	uint32_t sclk = 0, mclk = 0;
	uint16_t vddc = 0;
	int r;

	if (adev->si_dpm)
		return 0;
	if (!adev->atom)
		return -ENODEV;
	if (!adev->si_dpm_wanted) {
		frb_info(adev, "si_dpm: off (FirebrandSIDPM false / frb_sidpm=0): the card stays at its boot clocks");
		return -ENOSYS;
	}
	s = (struct frb_si_dpm *)adev->os->alloc(adev->os->cookie, sizeof(*s));
	if (!s)
		return -ENOMEM;
	memset(s, 0, sizeof(*s));
	r = chip_of(adev, &s->chip);
	if (r) {
		adev->os->free(adev->os->cookie, s);
		return r;
	}
	s->card.os = adev->os;
	s->card.atom.ctx = adev->atom;
	s->card.chip = &s->chip;
	s->card.device = adev->device;
	s->card.adev = adev;
	s->adev = adev;
	s->forced = AMD_DPM_FORCED_LEVEL_AUTO;

	s->dpm = rdn_si_dpm_create(&s->card, SI_SYS_PCIE_MASK);
	if (!s->dpm) {
		frb_err(adev, "si_dpm: the power tables did not parse: boot clocks kept");
		adev->os->free(adev->os->cookie, s);
		return -EINVAL;
	}
	rdn_si_dpm_describe(s->dpm);
	if (adev->si_dpm_quirks) {
		rdn_si_dpm_set_quirks(s->dpm, adev->si_dpm_quirks);
		frb_info(adev, "si_dpm: quirks 0x%x (RDN_SI_DPM_*, si_dpm/rdn_si_dpm.h)", adev->si_dpm_quirks);
	}
	rdn_si_dpm_set_msg_hook(s->dpm, si_dpm_msg_hook, s);

	/* si_dpm_init_microcode's choice; stage smu checked the same image */
	if (adev->asic_type == CHIP_VERDE) {
		name = rdn_si_dpm_wants_kicker_smc(s->dpm) ? "verde_k_smc.bin" : "verde_smc.bin";
	} else {
		/* the other SI chips: amdgpu's choice (frb_fw_select.c: Oland, Hainan, banks_k_2 included) */
		frb_fw_smc_name(other, sizeof(other), adev->asic_type, adev->device, adev->revision, 0);
		name = other;
	}
	r = adev->os->firmware(adev->os->cookie, name, &fw, &fw_size);
	if (r) {
		frb_err(adev, "si_dpm: no %s (%d): boot clocks kept", name, r);
		rdn_si_dpm_destroy(s->dpm);
		adev->os->free(adev->os->cookie, s);
		return r;
	}
	r = rdn_si_dpm_enable(s->dpm, fw, fw_size);
	if (r || !rdn_si_dpm_enabled(s->dpm)) {
		frb_err(adev, "si_dpm: enable failed (%d): boot clocks kept", r);
		rdn_si_dpm_disable(s->dpm);
		rdn_si_dpm_destroy(s->dpm);
		adev->os->free(adev->os->cookie, s);
		return r ? r : -EIO;
	}
	adev->si_dpm = s;
	/*
	 * si_dpm_hw_init's amdgpu_legacy_dpm_compute_clocks: amdgpu enables
	 * DPM before its DCE block is up, so the SMC goes from the boot state
	 * to the performance state with no display in the configuration (the
	 * display gap ignored), and a display that comes on later changes the
	 * state only if it changes the levels. The same here, whatever is lit
	 * already; the caller follows with the display's configuration
	 * (frb_display_dpm_changed). radeon, and RadeonNI, make this first
	 * switch with the display on (RDN_SI_DPM_RADEON_RULES).
	 */
	if (!(adev->si_dpm_quirks & RDN_SI_DPM_RADEON_RULES)) {
		struct amd_pp_display_configuration none;

		memset(&none, 0, sizeof(none));
		none.min_vblank_time = 0xffffffff;
		none.display_clk = adev->display ? adev->display->default_dispclk : 0;
		r = si_dpm_apply(s, &none, "hw_init, before any display");
		if (r || !rdn_si_dpm_enabled(s->dpm)) {
			frb_err(adev, "si_dpm: the first state switch failed (%d): boot clocks kept", r);
			adev->si_dpm = NULL;
			rdn_si_dpm_disable(s->dpm);
			rdn_si_dpm_destroy(s->dpm);
			adev->os->free(adev->os->cookie, s);
			return r ? r : -EIO;
		}
	}
	rdn_si_dpm_current_level(s->dpm, &level, &sclk, &mclk, &vddc);
	frb_info(adev, "si_dpm: on with %s; SMC tables at state 0x%05x soft 0x%05x; level %u: engine %u0 kHz, "
		 "memory %u0 kHz, %u mV, %d mC", name, rdn_si_dpm_table_address(s->dpm, 0),
		 rdn_si_dpm_table_address(s->dpm, 1), level, sclk, mclk, vddc, rdn_si_dpm_temperature(s->dpm));
	rdn_si_dpm_describe_voltage_table(s->dpm);
	return 0;
}

int frb_si_dpm_enabled(struct frb_device *adev)
{
	struct frb_si_dpm *s = adev->si_dpm;

	return s && rdn_si_dpm_enabled(s->dpm);
}

/* back to the boot state with DPM off (rdn_si_dpm_disable: si_dpm_disable), then freed */
void frb_si_dpm_fini(struct frb_device *adev)
{
	struct frb_si_dpm *s = adev->si_dpm;

	if (!s)
		return;
	rdn_si_dpm_disable(s->dpm);
	rdn_si_dpm_destroy(s->dpm);
	adev->os->free(adev->os->cookie, s);
	adev->si_dpm = NULL;
	frb_info(adev, "si_dpm: off (boot state, DPM disabled)");
}

/*
 * amdgpu_dpm_change_power_state_locked for the performance state with a
 * display configuration (amdgpu_dpm_get_display_cfg's): the active CRTCs,
 * the shortest vertical blank, the first CRTC's line time, the displays
 * above 297 MHz. The SMC then moves between the state's levels by load.
 */
static int si_dpm_apply(struct frb_si_dpm *s, const struct amd_pp_display_configuration *cfg, const char *when)
{
	struct frb_device *adev = s->adev;
	struct rdn_si_display disp;
	const char *why = NULL;
	uint32_t i, slo = 0, mlo = 0, shi = 0, mhi = 0;
	int r;

	memset(&disp, 0, sizeof(disp));
	for (i = 0; i < cfg->num_display && i < MAX_NUM_DISPLAY; i++) {
		disp.active_crtcs |= 1u << cfg->displays[i].controller_id;
		/* amdgpu's si_apply_state_adjust_rules: "high pixelclock" is above what 4K at 30 Hz needs */
		if (cfg->displays[i].pixel_clock > 297000)
			disp.high_pixelclock_count++;
	}
	disp.vblank_time_us = cfg->num_display ? cfg->min_vblank_time : 0xffffffff;
	disp.dispclk = cfg->display_clk;
	/* the DCE keeps the line time in ns (dce_v6_0 line_time) under amdgpu's _in_us name */
	disp.line_time = cfg->num_display ? cfg->line_time_in_us : 0;
	disp.wm_high = disp.wm_low = disp.line_time;
	rdn_si_dpm_display_changed(s->dpm, &disp);
	r = rdn_si_dpm_set_power_state(s->dpm, s->uvd_active ? RDN_SI_STATE_UVD : RDN_SI_STATE_PERFORMANCE);
	rdn_si_dpm_mclk_held(s->dpm, &why);
	rdn_si_dpm_clock_range(s->dpm, &slo, &mlo, &shi, &mhi);
	frb_info(adev, "si_dpm: %s: %u CRTC(s) (mask %x), vblank %u us, line %u ns, %u above 297 MHz: %s state "
		 "%s (%d), levels %u/%u .. %u/%u kHz; memory clock %s%s", when, cfg->num_display, disp.active_crtcs,
		 disp.vblank_time_us, disp.line_time, disp.high_pixelclock_count, s->uvd_active ? "UVD" : "performance",
		 r == 1 ? "already the SMC's, no switch" : r ? "FAILED" : "switched to", r, slo * 10, mlo * 10,
		 shi * 10, mhi * 10, why ? "held: " : "switching", why ? why : "");
	if (r < 0 && !rdn_si_dpm_enabled(s->dpm))
		frb_err(adev, "si_dpm: the SMC left the state switch unanswered and is stopped: DPM is off until the card "
			"is reset (docs/si-bring-up.md)");
	return r < 0 ? r : 0;
}

/* amdgpu_dpm_compute_clocks's legacy arm with the DCE's configuration; the caller has run the bandwidth update */
void frb_si_dpm_compute_clocks(struct frb_device *adev, const struct amd_pp_display_configuration *cfg)
{
	struct frb_si_dpm *s = adev->si_dpm;

	if (!s || !rdn_si_dpm_enabled(s->dpm))
		return;
	s->cfg = *cfg;
	s->have_cfg = 1;
	si_dpm_apply(s, cfg, "display");
}

/*
 * amdgpu_dpm_enable_uvd's SI arm: uvd_active and the state type
 * POWER_STATE_TYPE_INTERNAL_UVD on, or uvd_active off (the user's state
 * again), then amdgpu_dpm_compute_clocks with the display configuration
 * there is: the UVD state's levels with the engine and memory clocks held
 * (si_apply_state_adjust_rules: a state with VCLK/DCLK), its UPLL clocks set
 * around the switch (ni_set_uvd_clock_before/after_set_eng_clock).
 */
int frb_si_dpm_enable_uvd(struct frb_device *adev, int enable)
{
	struct frb_si_dpm *s = adev->si_dpm;
	struct amd_pp_display_configuration none;

	if (!s || !rdn_si_dpm_enabled(s->dpm))
		return -ENODEV;
	if (s->uvd_active == !!enable)
		return 0;
	s->uvd_active = !!enable;
	if (!s->have_cfg)
		memset(&none, 0, sizeof(none));
	return si_dpm_apply(s, s->have_cfg ? &s->cfg : &none, enable ? "UVD on" : "UVD off");
}

int frb_si_dpm_force_performance_level(struct frb_device *adev, enum amd_dpm_forced_level level)
{
	struct frb_si_dpm *s = adev->si_dpm;
	int r;

	if (!s || !rdn_si_dpm_enabled(s->dpm))
		return -ENODEV;
	if (adev->si_dpm_quirks & RDN_SI_DPM_SWITCH_TRACE) {
		struct si_sw_trace *t = &s->sw;
		uint32_t i;

		memset(t, 0, sizeof(*t));
		for (i = 0; i < SW_NREGS; i++)
			t->last[i] = sw_rd(adev, sw_regs[i].off);
		t->pc_last = sw_smc_pc(adev);
		t->pos_min = 0xffffffff;
		t->in_vblank = sw_rd(adev, SI_CRTC_STATUS) & 1;
		t->t0 = adev->os->time_ms(adev->os->cookie);
	}
	switch (level) {
	case AMD_DPM_FORCED_LEVEL_AUTO:
		r = rdn_si_dpm_force_performance_level(s->dpm, RDN_SI_FORCED_LEVEL_AUTO);
		break;
	case AMD_DPM_FORCED_LEVEL_LOW:
		r = rdn_si_dpm_force_performance_level(s->dpm, RDN_SI_FORCED_LEVEL_LOW);
		break;
	case AMD_DPM_FORCED_LEVEL_HIGH:
		r = rdn_si_dpm_force_performance_level(s->dpm, RDN_SI_FORCED_LEVEL_HIGH);
		break;
	default:
		return -EINVAL;
	}
	if (!r)
		s->forced = level;
	/*
	 * SWITCH_TRACE: the level change itself is the SMC's own, after its
	 * answers: watched for 400 ms as a state switch is.
	 */
	if (adev->si_dpm_quirks & RDN_SI_DPM_SWITCH_TRACE) {
		struct si_sw_trace *t = &s->sw;
		uint32_t i;

		for (i = 0; i < 2000; i++) {
			sw_sample(s, i);
			adev->os->delay_us(adev->os->cookie, 150);
			if (adev->os->time_ms(adev->os->cookie) - t->t0 > 400)
				break;
		}
		frb_info(adev, "si_dpm: forced level %d (%d): %u samples over %u ms, %u changes (%u more not kept); %u vblanks "
			 "seen, lines %u..%u; SMC pc %u changes, last %08x", (int)level, r, t->samples,
			 (uint32_t)(adev->os->time_ms(adev->os->cookie) - t->t0), t->nev, t->lost, t->vblank_edges,
			 t->pos_min, t->pos_max, t->pc_changes, t->pc_last);
		for (i = 0; i < t->nev; i++)
			frb_info(adev, "si_dpm:   +%u ms (sample %u): %s %08x -> %08x", t->ev[i].ms, t->ev[i].count,
				 sw_regs[t->ev[i].reg].name, t->ev[i].from, t->ev[i].to);
	}
	return r;
}

/* the performance state's lowest or highest level, 10 kHz (radeon's dpm_get_sclk/mclk) */
uint32_t frb_si_dpm_get_sclk(struct frb_device *adev, int low)
{
	struct frb_si_dpm *s = adev->si_dpm;
	uint32_t slo = 0, mlo = 0, shi = 0, mhi = 0;

	if (!s || !rdn_si_dpm_enabled(s->dpm))
		return 0;
	rdn_si_dpm_clock_range(s->dpm, &slo, &mlo, &shi, &mhi);
	return low ? slo : shi;
}

uint32_t frb_si_dpm_get_mclk(struct frb_device *adev, int low)
{
	struct frb_si_dpm *s = adev->si_dpm;
	uint32_t slo = 0, mlo = 0, shi = 0, mhi = 0;

	if (!s || !rdn_si_dpm_enabled(s->dpm))
		return 0;
	rdn_si_dpm_clock_range(s->dpm, &slo, &mlo, &shi, &mhi);
	return low ? mlo : mhi;
}

/* frbctl dpm: the level the SMC is at, its clocks and voltage, the temperature and the fan */
int frb_si_dpm_get_status(struct frb_device *adev, struct frb_pm_status *st)
{
	struct frb_si_dpm *s = adev->si_dpm;
	unsigned level = 0;
	uint32_t sclk = 0, mclk = 0, slo = 0, mlo = 0, shi = 0, mhi = 0;
	uint16_t vddc = 0;
	int pct;

	memset(st, 0, sizeof(*st));
	if (!s || !rdn_si_dpm_enabled(s->dpm))
		return -ENODEV;
	st->enabled = 1;
	st->forced_level = s->forced;
	rdn_si_dpm_clock_range(s->dpm, &slo, &mlo, &shi, &mhi);
	st->sclk_count = 2;
	st->mclk_count = 2;
	st->sclk_levels[0] = slo;
	st->sclk_levels[1] = shi;
	st->mclk_levels[0] = mlo;
	st->mclk_levels[1] = mhi;
	st->sclk_mask = st->mclk_mask = 3;
	if (!rdn_si_dpm_current_level(s->dpm, &level, &sclk, &mclk, &vddc)) {
		st->cur_sclk = sclk;
		st->cur_mclk = mclk;
		st->cur_sclk_index = (int32_t)level;
		st->cur_mclk_index = (int32_t)level;
		st->vddgfx_mv = vddc;
	} else {
		st->cur_sclk_index = st->cur_mclk_index = -1;
	}
	st->temp_mc = rdn_si_dpm_temperature(s->dpm);
	pct = rdn_si_dpm_fan_percent(s->dpm);
	if (pct >= 0)
		st->fan_pwm = (uint32_t)pct * 255 / 100;
	if (s->have_cfg) {
		st->num_display = s->cfg.num_display;
		st->min_vblank_time = s->cfg.min_vblank_time;
	}
	return 0;
}
