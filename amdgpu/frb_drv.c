/*
 * The family table: amdgpu_drv.c's pciidlist for the Polaris and Vega 20
 * parts, and the rows for the cards this driver has actually met.
 *
 * Two levels, on purpose:
 * - frb_asic_type_of() is Linux's table: device id -> CHIP_POLARIS1x. It is
 *   what the survey reports and what decides firmware names, and it never
 *   by itself lets the driver write a register.
 * - frb_asic_lookup() finds a CARD row (device, revision, subsystem). Only a
 *   row may raise the bring-up past the survey, and only up to its
 *   max_stage: the highest stage verified on that card against its Linux
 *   reference capture. A Polaris with no row is surveyed and left alone.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_device.h"

/* drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c, pciidlist: the SI, Polaris and Vega 20 rows */
static const struct { uint16_t device; enum amd_asic_type type; } pciidlist[] = {
	/* Tahiti */
	{ 0x6780, CHIP_TAHITI }, { 0x6784, CHIP_TAHITI }, { 0x6788, CHIP_TAHITI },
	{ 0x678A, CHIP_TAHITI }, { 0x6790, CHIP_TAHITI }, { 0x6791, CHIP_TAHITI },
	{ 0x6792, CHIP_TAHITI }, { 0x6798, CHIP_TAHITI }, { 0x6799, CHIP_TAHITI },
	{ 0x679A, CHIP_TAHITI }, { 0x679B, CHIP_TAHITI }, { 0x679E, CHIP_TAHITI },
	{ 0x679F, CHIP_TAHITI },
	/* Pitcairn */
	{ 0x6800, CHIP_PITCAIRN }, { 0x6801, CHIP_PITCAIRN }, { 0x6802, CHIP_PITCAIRN },
	{ 0x6806, CHIP_PITCAIRN }, { 0x6808, CHIP_PITCAIRN }, { 0x6809, CHIP_PITCAIRN },
	{ 0x6810, CHIP_PITCAIRN }, { 0x6811, CHIP_PITCAIRN }, { 0x6816, CHIP_PITCAIRN },
	{ 0x6817, CHIP_PITCAIRN }, { 0x6818, CHIP_PITCAIRN }, { 0x6819, CHIP_PITCAIRN },
	/* Oland */
	{ 0x6600, CHIP_OLAND }, { 0x6601, CHIP_OLAND }, { 0x6602, CHIP_OLAND },
	{ 0x6603, CHIP_OLAND }, { 0x6604, CHIP_OLAND }, { 0x6605, CHIP_OLAND },
	{ 0x6606, CHIP_OLAND }, { 0x6607, CHIP_OLAND }, { 0x6608, CHIP_OLAND },
	{ 0x6610, CHIP_OLAND }, { 0x6611, CHIP_OLAND }, { 0x6613, CHIP_OLAND },
	{ 0x6617, CHIP_OLAND }, { 0x6620, CHIP_OLAND }, { 0x6621, CHIP_OLAND },
	{ 0x6623, CHIP_OLAND }, { 0x6631, CHIP_OLAND },
	/* Verde */
	{ 0x6820, CHIP_VERDE }, { 0x6821, CHIP_VERDE }, { 0x6822, CHIP_VERDE },
	{ 0x6823, CHIP_VERDE }, { 0x6824, CHIP_VERDE }, { 0x6825, CHIP_VERDE },
	{ 0x6826, CHIP_VERDE }, { 0x6827, CHIP_VERDE }, { 0x6828, CHIP_VERDE },
	{ 0x6829, CHIP_VERDE }, { 0x682A, CHIP_VERDE }, { 0x682B, CHIP_VERDE },
	{ 0x682C, CHIP_VERDE }, { 0x682D, CHIP_VERDE }, { 0x682F, CHIP_VERDE },
	{ 0x6830, CHIP_VERDE }, { 0x6831, CHIP_VERDE }, { 0x6835, CHIP_VERDE },
	{ 0x6837, CHIP_VERDE }, { 0x6838, CHIP_VERDE }, { 0x6839, CHIP_VERDE },
	{ 0x683B, CHIP_VERDE }, { 0x683D, CHIP_VERDE }, { 0x683F, CHIP_VERDE },
	/* Hainan */
	{ 0x6660, CHIP_HAINAN }, { 0x6663, CHIP_HAINAN }, { 0x6664, CHIP_HAINAN },
	{ 0x6665, CHIP_HAINAN }, { 0x6667, CHIP_HAINAN }, { 0x666F, CHIP_HAINAN },
	/* Polaris11 */
	{ 0x67E0, CHIP_POLARIS11 }, { 0x67E3, CHIP_POLARIS11 }, { 0x67E8, CHIP_POLARIS11 },
	{ 0x67EB, CHIP_POLARIS11 }, { 0x67EF, CHIP_POLARIS11 }, { 0x67FF, CHIP_POLARIS11 },
	{ 0x67E1, CHIP_POLARIS11 }, { 0x67E7, CHIP_POLARIS11 }, { 0x67E9, CHIP_POLARIS11 },
	/* Polaris10 */
	{ 0x67C0, CHIP_POLARIS10 }, { 0x67C1, CHIP_POLARIS10 }, { 0x67C2, CHIP_POLARIS10 },
	{ 0x67C4, CHIP_POLARIS10 }, { 0x67C7, CHIP_POLARIS10 }, { 0x67C8, CHIP_POLARIS10 },
	{ 0x67C9, CHIP_POLARIS10 }, { 0x67CA, CHIP_POLARIS10 }, { 0x67CC, CHIP_POLARIS10 },
	{ 0x67CF, CHIP_POLARIS10 }, { 0x67D0, CHIP_POLARIS10 }, { 0x67DF, CHIP_POLARIS10 },
	{ 0x6FDF, CHIP_POLARIS10 },
	/* Polaris12 */
	{ 0x6980, CHIP_POLARIS12 }, { 0x6981, CHIP_POLARIS12 }, { 0x6985, CHIP_POLARIS12 },
	{ 0x6986, CHIP_POLARIS12 }, { 0x6987, CHIP_POLARIS12 }, { 0x6995, CHIP_POLARIS12 },
	{ 0x6997, CHIP_POLARIS12 }, { 0x699F, CHIP_POLARIS12 },
	/* VegaM */
	{ 0x694C, CHIP_VEGAM }, { 0x694E, CHIP_VEGAM }, { 0x694F, CHIP_VEGAM },
	/* Vega 20 */
	{ 0x66A0, CHIP_VEGA20 }, { 0x66A1, CHIP_VEGA20 }, { 0x66A2, CHIP_VEGA20 },
	{ 0x66A3, CHIP_VEGA20 }, { 0x66A4, CHIP_VEGA20 }, { 0x66A7, CHIP_VEGA20 },
	{ 0x66AF, CHIP_VEGA20 },
};

/*
 * Cards met so far. Both are Polaris 12 "Lexa", not "kicker" parts
 * (ASICID_IS_P23 is 6987/6981 only), so polaris12_smc.bin and the
 * polaris12_mc.bin / polaris12_32_mc.bin choice gmc_v8_0 makes at run time.
 *
 * E9173: checked against a Linux amdgpu register capture (2026-11-13).
 * RX 550: its own Linux capture (2026-11-11); same
 *   firmware and the same 90-message SMU sequence. Stages 1-3 verified
 *   2026-11-13 against that capture (FirebrandBringUp run).
 * Stages dce, gfx and sdma on both cards (2026-11-13), docs/stage-verification.md:
 *   dce against Linux 6.12's legacy display source call by call, the end
 *   state DC leaves in the same registers at the same mode, and the sink's
 *   CRC of every probe frame; gfx and sdma against both captures (replays
 *   and fresh-card traces) and on the hardware.
 * Stage uvd on the E9173 (2026-11-14): the replay against its capture (with
 *   and without DPM), the ring/IB/swap tests on the card, 12 H.264 vectors
 *   bit-exact against ffmpeg, FrostByte playback (docs/stage-verification.md).
 *   On the RX 550 the same day: the ring/IB/swap tests and the 12 vectors.
 */
/*
 * R7 450 (under Firebrand since 2026-11-15; under RadeonNI before): Dell's
 * Cape Verde PRO, 682b rev 87, subsystem 1028:1004, 4 GB 128-bit GDDR5
 * (si58_mc.bin: MC_SEQ_MISC0[31:24] = 0x58), DP, DP, DVI-I. The "kicker"
 * SMC image (verde_k_smc.bin). No amdgpu capture: the references are
 * radeon's end-state registers and SMC SRAM on this card
 * (a Linux radeon capture, 2026-11-11) and RadeonNI's proven
 * sequences on it (RadeonNI's hw/rdn_si.c, rdn_modeset.c).
 * Stages post (cold ASIC_Init), gmc (MC microcode trained as RadeonNI from
 * cold, VRAM, GART) and smu (the SMC image) verified 2026-11-15;
 * dce the same day with the LG on DP connector 0: EDID and DPCD over AUX,
 * 3840x2160@60 trained at HBR2 x4, the scanout read back equal to the host
 * model, the sink CRC, the 64x64 hardware cursor seen right
 * (docs/si-bring-up.md).
 */
/*
 * MI50: checked against a Linux amdgpu register capture (2026-11-13).
 * Stages 1-3 (post, gmc, smu
 * with the PSP's firmware loads and powerplay DPM) verified
 * 2026-11-14 against that capture: every write Linux makes, in order, but
 * the addresses of buffers Firebrand places itself; 127 SMU messages and
 * their 104 answers equal; the PPTable in VRAM equal byte for byte
 * (docs/vega20-bring-up.md). Stage dce (DCE 12) verified
 * 2026-11-14 with the LG 4K on the mini-DP: DC's init, detection, first mode
 * set, DPMS off/on and plane update replay equal little- and big-endian; on
 * the card the pipe and sink CRCs of libdrm's SMPTE bars equal Linux's, the
 * hardware cursor equals a software reference, and the window server's
 * desktop runs at 3840x2160@60. At stage dce the row joins the release
 * list, so a BRINGUP=vega20 build (kext Makefile, which also embeds the
 * vega20 microcode) is installed and autoloads.
 * Stages gfx and sdma (gfx v9, sdma v4) verified 2026-11-15:
 * the replays equal Linux's writes in order little- and big-endian (the
 * KIQ/compute queues and SDMA page queues not ported, passed over), the
 * rings after the stages equal Linux's debugfs rings, and on the card the
 * gfx ring and IB tests and both SDMA engines' ring, IB, copy and fill
 * tests pass on doorbells (a fresh-card trace compare of these stages is
 * still to do).
 * Stage uvd (uvd v7, both instances) verified 2026-11-15: the
 * replay equals Linux's uvd_v7_0 hw_init, ring tests, IB tests and idle work
 * little- and big-endian (the encode rings and VCE not ported, cut); on the
 * card both VCPUs start from the PSP-loaded image, the ring and IB tests
 * pass, the stage's trace has every Linux write in order (92 identical, 18
 * placement), the LMI swap probe passes both ways, and through 'rdnv' the 12
 * H.264 vectors equal ffmpeg's md5 (docs/vega20-bring-up.md, "Stage uvd").
 */
static const struct frb_asic_row rows[] = {
	{ 0x682B, 0x1028, 0x1004, 0x87, CHIP_VERDE, "VERDE", "verde",
	  "AMD Radeon R7 450 (Cape Verde, 4 GB 128-bit GDDR5, DP/DP/DVI-I)", FRB_STAGE_DCE },
	{ 0x699F, 0x1002, 0xEA73, 0x81, CHIP_POLARIS12, "POLARIS12", "polaris12",
	  "AMD Embedded Radeon E9173 (Lexa XL, 2 GB 64-bit GDDR5, 3x DP)", FRB_STAGE_UVD },
	{ 0x699F, 0x1002, 0x0B04, 0xC7, CHIP_POLARIS12, "POLARIS12", "polaris12",
	  "Radeon RX 550 (Lexa, 4 GB 128-bit GDDR5, DP/HDMI/DVI)", FRB_STAGE_UVD },
	{ 0x66A1, 0x1002, 0x081E, 0x06, CHIP_VEGA20, "VEGA20", "vega20",
	  "AMD Instinct MI50 (Vega 20, 16 GB HBM2, Radeon Pro VII VBIOS, passive)", FRB_STAGE_UVD,
	  FRB_QUIRK_AUDIO_FN_STRAP },
};

enum amd_asic_type frb_asic_type_of(uint16_t device)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pciidlist); i++)
		if (pciidlist[i].device == device)
			return pciidlist[i].type;
	return CHIP_UNKNOWN_FRB;
}

const char *frb_asic_name(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_TAHITI:	return "TAHITI";
	case CHIP_PITCAIRN:	return "PITCAIRN";
	case CHIP_VERDE:	return "VERDE";
	case CHIP_OLAND:	return "OLAND";
	case CHIP_HAINAN:	return "HAINAN";
	case CHIP_POLARIS10:	return "POLARIS10";
	case CHIP_POLARIS11:	return "POLARIS11";
	case CHIP_POLARIS12:	return "POLARIS12";
	case CHIP_VEGAM:	return "VEGAM";
	case CHIP_VEGA10:	return "VEGA10";
	case CHIP_VEGA12:	return "VEGA12";
	case CHIP_VEGA20:	return "VEGA20";
	default:		return "unknown";
	}
}

const struct frb_asic_row *frb_asic_lookup(uint16_t device, uint8_t revision,
					   uint16_t subsys_vendor, uint16_t subsys_device)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		const struct frb_asic_row *r = &rows[i];

		if (r->device != device || r->revision != revision)
			continue;
		if (r->subsys_vendor && r->subsys_vendor != subsys_vendor)
			continue;
		if (r->subsys_device && r->subsys_device != subsys_device)
			continue;
		return r;
	}
	return 0;
}

/* si_set_ip_blocks / vi_set_ip_blocks / soc15_set_ip_blocks: the family's table */
const struct frb_asic_funcs *frb_asic_funcs_of(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_TAHITI:
	case CHIP_PITCAIRN:
	case CHIP_VERDE:
	case CHIP_OLAND:
	case CHIP_HAINAN:
		return &frb_si_asic_funcs;
	case CHIP_POLARIS10:
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
	case CHIP_VEGAM:
		return &frb_vi_asic_funcs;
	case CHIP_VEGA10:
	case CHIP_VEGA12:
	case CHIP_VEGA20:
		return &frb_soc15_asic_funcs;
	default:
		return 0;
	}
}
