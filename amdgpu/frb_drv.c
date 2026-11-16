/*
 * The family table: amdgpu_drv.c's pciidlist for the SI, CIK, Polaris and
 * Vega parts, and the rows for the cards this driver has actually met.
 *
 * Two levels, on purpose:
 * - frb_asic_type_of() is Linux's table: device id -> CHIP_*. It is what
 *   the survey reports and what decides firmware names.
 * - frb_asic_lookup() finds a CARD row (device, revision, subsystem): a card
 *   verified on hardware, up to its max_stage, the highest stage verified on
 *   that card.
 * A card of a ported family with no row of its own is an UNTESTED card
 * (frb_asic_untested_row): it runs display-only (stage dce) unless unlocked
 * (frb_stage_cap; `sudo frbctl unlock`), and its report goes to a GitHub
 * issue. Vega 10 and Vega 12 are recognised but not ported (frb_asic_ported):
 * their VBIOS is read for the report, their registers are never touched.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_device.h"

/*
 * drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c (Linux 7.1), pciidlist: every
 * discrete SI, CIK, Polaris and Vega row. The kext's IOPCIMatch is made from
 * this table (kext/Firebrand/Makefile), so every card here matches.
 */
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
	/*
	 * Bonaire, Hawaii: the CIK discrete parts (Kaveri, Kabini and Mullins are
	 * APUs, which no board here can hold). RadeonNI matches the Hawaii ids.
	 */
	{ 0x6640, CHIP_BONAIRE }, { 0x6641, CHIP_BONAIRE }, { 0x6646, CHIP_BONAIRE },
	{ 0x6647, CHIP_BONAIRE }, { 0x6649, CHIP_BONAIRE }, { 0x6650, CHIP_BONAIRE },
	{ 0x6651, CHIP_BONAIRE }, { 0x6658, CHIP_BONAIRE }, { 0x665C, CHIP_BONAIRE },
	{ 0x665D, CHIP_BONAIRE }, { 0x665F, CHIP_BONAIRE },
	{ 0x67A0, CHIP_HAWAII }, { 0x67A1, CHIP_HAWAII }, { 0x67A2, CHIP_HAWAII },
	{ 0x67A8, CHIP_HAWAII }, { 0x67A9, CHIP_HAWAII }, { 0x67AA, CHIP_HAWAII },
	{ 0x67B0, CHIP_HAWAII }, { 0x67B1, CHIP_HAWAII }, { 0x67B8, CHIP_HAWAII },
	{ 0x67B9, CHIP_HAWAII }, { 0x67BA, CHIP_HAWAII }, { 0x67BE, CHIP_HAWAII },
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
	/* Vega 10 (recognised only: frb_asic_ported) */
	{ 0x6860, CHIP_VEGA10 }, { 0x6861, CHIP_VEGA10 }, { 0x6862, CHIP_VEGA10 },
	{ 0x6863, CHIP_VEGA10 }, { 0x6864, CHIP_VEGA10 }, { 0x6867, CHIP_VEGA10 },
	{ 0x6868, CHIP_VEGA10 }, { 0x6869, CHIP_VEGA10 }, { 0x686A, CHIP_VEGA10 },
	{ 0x686B, CHIP_VEGA10 }, { 0x686C, CHIP_VEGA10 }, { 0x686D, CHIP_VEGA10 },
	{ 0x686E, CHIP_VEGA10 }, { 0x686F, CHIP_VEGA10 }, { 0x687F, CHIP_VEGA10 },
	/* Vega 12 (recognised only) */
	{ 0x69A0, CHIP_VEGA12 }, { 0x69A1, CHIP_VEGA12 }, { 0x69A2, CHIP_VEGA12 },
	{ 0x69A3, CHIP_VEGA12 }, { 0x69AF, CHIP_VEGA12 },
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
 * (docs/si-bring-up.md). Stages gfx and sdma (gfx v6, si_dma) the same day:
 * ring, IB, copy and fill tests on both DMA engines, the accelerator's 2D
 * tests, Quartz Extreme, Core Image and GL 3.3/4.5 through the GL bundle,
 * clock gating, and si_dpm (the SMC's tables equal to Linux radeon's, the
 * levels 300/150 .. 925/1125 MHz switching forced and by load).
 * Stage uvd (UVD 3.1) 2026-11-15: the firmware validated, the VCPU up, ring
 * and IB tests, and through 'rdnv' (the legacy H.264 messages, every
 * buffer in the VCPU bo's 256 MB segment) the 12 H.264 vectors equal
 * ffmpeg's md5 without DPM and with it (si_dpm's UVD state, its UPLL
 * clocks); FrostByte playback (docs/si-bring-up.md, "UVD 3.1").
 */
/*
 * W8100 (under Firebrand since 2026-11-15; under RadeonNI before):
 * Dell's FirePro W8100, Hawaii GL40 A0, 67a1 rev 00, subsystem 1028:030c,
 * 8 GB 512-bit GDDR5, four DP 1.2 (VBIOS C67502, 113-C6750200-100). No
 * amdgpu capture: the references are RadeonNI's measured CIK bring-up on
 * this card (RadeonNI's hw/rdn_si.c, rdn_cik_smc.c, rdn_ci_dpm.c) and
 * Linux radeon's DPM register and SMU-table snapshots of it (2026-11-04). Stages post (cold
 * ASIC_Init), gmc (hawaii_mc.bin trained as RadeonNI from cold, all 8 GB
 * at 0xF4_0000_0000, GART) and smu verified 2026-11-15, every
 * value equal to RadeonNI's log on this card; CI DPM right after stage smu,
 * the SMC's tables equal to Linux radeon's and the levels 300/150 ..
 * 824/1250 MHz switching as Linux's do (docs/cik-bring-up.md). Stages dce
 * (DCE 8), gfx (gfx v7) and sdma (cik_sdma) the same evening: the four DP
 * paths as RadeonNI logged them, the LG at 4K60 HBR2 x4 and the desktop at
 * 2560x1440, the ring/IB/copy/fill/GTT tests, the accelerator, Quartz
 * Extreme, Core Image and GL 4.5 through the shared bundle; the picture and
 * the cursor checked by eye and DP audio heard. Stage uvd (uvd v4.2) the same
 * night: the VCPU up with CI DPM's UVD levels, the ring, IB and idle tests,
 * and through 'rdnv' the 12 H.264 vectors equal ffmpeg's md5 (5.4 ms a 1080p
 * picture in the decoder).
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
	  "AMD Radeon R7 450 (Cape Verde, 4 GB 128-bit GDDR5, DP/DP/DVI-I)", FRB_STAGE_UVD },
	{ 0x67A1, 0x1028, 0x030C, 0x00, CHIP_HAWAII, "HAWAII", "hawaii",
	  "AMD FirePro W8100 (Hawaii, 8 GB 512-bit GDDR5, 4x DP)", FRB_STAGE_UVD },
	{ 0x699F, 0x1002, 0xEA73, 0x81, CHIP_POLARIS12, "POLARIS12", "polaris12",
	  "AMD Embedded Radeon E9173 (Lexa XL, 2 GB 64-bit GDDR5, 3x DP)", FRB_STAGE_UVD },
	{ 0x699F, 0x1002, 0x0B04, 0xC7, CHIP_POLARIS12, "POLARIS12", "polaris12",
	  "Radeon RX 550 (Lexa, 4 GB 128-bit GDDR5, DP/HDMI/DVI)", FRB_STAGE_UVD },
	{ 0x66A1, 0x1002, 0x081E, 0x06, CHIP_VEGA20, "VEGA20", "vega20",
	  "AMD Instinct MI50 (Vega 20, 16 GB HBM2, Radeon Pro VII VBIOS, passive)", FRB_STAGE_UVD,
	  FRB_QUIRK_AUDIO_FN_STRAP },
};

int frb_verified_row_count(void)
{
	return (int)ARRAY_SIZE(rows);
}

const struct frb_asic_row *frb_verified_row(int i)
{
	return i >= 0 && i < (int)ARRAY_SIZE(rows) ? &rows[i] : 0;
}

int frb_pciid_count(void)
{
	return (int)ARRAY_SIZE(pciidlist);
}

uint16_t frb_pciid_device(int i)
{
	return i >= 0 && i < (int)ARRAY_SIZE(pciidlist) ? pciidlist[i].device : 0;
}

enum amd_asic_type frb_asic_type_of(uint16_t device)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pciidlist); i++)
		if (pciidlist[i].device == device)
			return pciidlist[i].type;
	return CHIP_UNKNOWN_FRB;
}

/* amdgpu_drv.c asic_type_quirks: Polaris 10 and 11 parts with the same device id */
static const struct { uint16_t device; uint8_t revision; enum amd_asic_type type; } asic_type_quirks[] = {
	{ 0x67FF, 0xE3, CHIP_POLARIS10 },
	{ 0x67FF, 0xE7, CHIP_POLARIS10 },
	{ 0x67FF, 0xF3, CHIP_POLARIS10 },
	{ 0x67FF, 0xF7, CHIP_POLARIS10 },
};

enum amd_asic_type frb_asic_type_of_rev(uint16_t device, uint8_t revision)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(asic_type_quirks); i++)
		if (asic_type_quirks[i].device == device && asic_type_quirks[i].revision == revision)
			return asic_type_quirks[i].type;
	return frb_asic_type_of(device);
}

int frb_asic_ported(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_VEGA10:
	case CHIP_VEGA12:
	case CHIP_UNKNOWN_FRB:
		return 0;
	default:
		return frb_asic_funcs_of(t) != 0;
	}
}

/* the linux-firmware prefix and the words for an untested card of each chip */
static const struct { enum amd_asic_type type; const char *fw_prefix, *board; } untested_names[] = {
	{ CHIP_TAHITI,		"tahiti",	"AMD Tahiti GPU (Southern Islands, untested card)" },
	{ CHIP_PITCAIRN,	"pitcairn",	"AMD Pitcairn GPU (Southern Islands, untested card)" },
	{ CHIP_VERDE,		"verde",	"AMD Cape Verde GPU (Southern Islands, untested card)" },
	{ CHIP_OLAND,		"oland",	"AMD Oland GPU (Southern Islands, untested card)" },
	{ CHIP_HAINAN,		"hainan",	"AMD Hainan GPU (Southern Islands, untested card)" },
	{ CHIP_BONAIRE,		"bonaire",	"AMD Bonaire GPU (Sea Islands, untested card)" },
	{ CHIP_HAWAII,		"hawaii",	"AMD Hawaii GPU (Sea Islands, untested card)" },
	{ CHIP_POLARIS10,	"polaris10",	"AMD Polaris 10 GPU (untested card)" },
	{ CHIP_POLARIS11,	"polaris11",	"AMD Polaris 11 GPU (untested card)" },
	{ CHIP_POLARIS12,	"polaris12",	"AMD Polaris 12 GPU (untested card)" },
	{ CHIP_VEGAM,		"vegam",	"AMD Vega M GPU (untested card)" },
	{ CHIP_VEGA10,		"vega10",	"AMD Vega 10 GPU (not ported yet)" },
	{ CHIP_VEGA12,		"vega12",	"AMD Vega 12 GPU (not ported yet)" },
	{ CHIP_VEGA20,		"vega20",	"AMD Vega 20 GPU (untested card)" },
};

void frb_asic_untested_row(struct frb_device *adev)
{
	struct frb_asic_row *r = &adev->untested_row;
	unsigned int i;

	r->device = adev->device;
	r->subsys_vendor = adev->subsys_vendor;
	r->subsys_device = adev->subsys_device;
	r->revision = adev->revision;
	r->asic_type = adev->asic_type;
	r->asic_name = frb_asic_name(adev->asic_type);
	r->fw_prefix = "unknown";
	r->board = "AMD GPU (untested card)";
	for (i = 0; i < ARRAY_SIZE(untested_names); i++)
		if (untested_names[i].type == adev->asic_type) {
			r->fw_prefix = untested_names[i].fw_prefix;
			r->board = untested_names[i].board;
		}
	/* a chip Firebrand only recognises runs nothing (the kext does not even map its registers) */
	r->max_stage = frb_asic_ported(adev->asic_type) ? FRB_UNTESTED_STAGE : FRB_STAGE_SURVEY;
	r->quirks = 0;
	r->untested = 1;
	adev->row = r;
}

enum frb_stage frb_stage_cap(const struct frb_device *adev)
{
	if (!adev->row)
		return FRB_STAGE_SURVEY;
	if (adev->row->untested && adev->unlocked && frb_asic_ported(adev->asic_type))
		return FRB_UNLOCKED_STAGE;
	return (enum frb_stage)adev->row->max_stage;
}

const char *frb_asic_name(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_TAHITI:	return "TAHITI";
	case CHIP_PITCAIRN:	return "PITCAIRN";
	case CHIP_VERDE:	return "VERDE";
	case CHIP_OLAND:	return "OLAND";
	case CHIP_HAINAN:	return "HAINAN";
	case CHIP_BONAIRE:	return "BONAIRE";
	case CHIP_HAWAII:	return "HAWAII";
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

#ifdef FRB_FORCE_UNTESTED
	/* a test build (kext Makefile UNTESTED=0xDEVICE): that card's verified rows are ignored */
	if (device == FRB_FORCE_UNTESTED)
		return 0;
#endif
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

/* si_set_ip_blocks / cik_set_ip_blocks / vi_set_ip_blocks / soc15_set_ip_blocks: the family's table */
const struct frb_asic_funcs *frb_asic_funcs_of(enum amd_asic_type t)
{
	switch (t) {
	case CHIP_TAHITI:
	case CHIP_PITCAIRN:
	case CHIP_VERDE:
	case CHIP_OLAND:
	case CHIP_HAINAN:
		return &frb_si_asic_funcs;
	case CHIP_BONAIRE:
	case CHIP_HAWAII:
		return &frb_cik_asic_funcs;
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
